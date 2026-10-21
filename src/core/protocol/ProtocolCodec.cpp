#include "protocol/ProtocolCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <type_traits>
#include <utility>

namespace blendnet::protocol {
namespace {

using Json = nlohmann::json;
struct InvalidPayload {};
struct InvalidJson {};

template <typename T> CodecResult<T> failure(ProtocolError error) { return {std::nullopt, error}; }

template <typename T> CodecResult<T> success(T value) {
    return {std::move(value), ProtocolError::None};
}

bool knownType(MessageType type) { return toString(type) != "UNKNOWN"; }

ProtocolError validateHeader(const FrameHeader& header) {
    if (!knownType(header.type)) {
        return ProtocolError::UnknownMessageType;
    }
    const auto limit = header.type == MessageType::FileData ? maxFileDataBytes : maxControlBytes;
    if (header.payloadBytes > limit) {
        return ProtocolError::PayloadTooLarge;
    }
    return ProtocolError::None;
}

std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | std::uint32_t(bytes[offset + 3]);
}

void write32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void fields(const Json& data, std::initializer_list<std::string_view> names) {
    if (!data.is_object() || data.size() != names.size()) {
        throw InvalidPayload{};
    }
    for (const auto name : names) {
        if (!data.contains(name)) {
            throw InvalidPayload{};
        }
    }
}

std::string text(const Json& data, std::string_view key) {
    const auto& value = data.at(key);
    if (!value.is_string()) {
        throw InvalidPayload{};
    }
    auto result = value.get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos) {
        throw InvalidPayload{};
    }
    return result;
}

std::string choice(const Json& data, std::string_view key,
                   std::initializer_list<std::string_view> choices) {
    auto result = text(data, key);
    if (std::find(choices.begin(), choices.end(), result) == choices.end()) {
        throw InvalidPayload{};
    }
    return result;
}

int integer(const Json& data, std::string_view key, int minimum = std::numeric_limits<int>::min(),
            int maximum = std::numeric_limits<int>::max()) {
    const auto& value = data.at(key);
    if (!value.is_number_integer() || value < minimum || value > maximum) {
        throw InvalidPayload{};
    }
    return value.get<int>();
}

std::uint64_t unsignedInteger(const Json& data, std::string_view key,
                              std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max(),
                              std::uint64_t minimum = 0) {
    const auto& value = data.at(key);
    if (!value.is_number_integer() || value < minimum || value > maximum) {
        throw InvalidPayload{};
    }
    return value.get<std::uint64_t>();
}

std::uint32_t unsigned32(const Json& data, std::string_view key,
                         std::uint32_t maximum = std::numeric_limits<std::uint32_t>::max(),
                         std::uint32_t minimum = 0) {
    return static_cast<std::uint32_t>(unsignedInteger(data, key, maximum, minimum));
}

std::optional<int> optionalInteger(const Json& data, std::string_view key, int minimum = 0) {
    if (data.at(key).is_null()) {
        return std::nullopt;
    }
    return integer(data, key, minimum);
}

std::string fileId(const Json& data) {
    auto result = text(data, "fileId");
    if (result.size() > maxFileIdBytes) {
        throw InvalidPayload{};
    }
    return result;
}

std::string checksum(const Json& data) {
    auto result = text(data, "sha256");
    if (result.size() != 64 || !std::all_of(result.begin(), result.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                   (value >= 'A' && value <= 'F');
        })) {
        throw InvalidPayload{};
    }
    return result;
}

std::string engine(const Json& data, std::string_view key = "engine") {
    return choice(data, key, {"CYCLES", "EEVEE"});
}

std::string outputFormat(const Json& data) {
    return choice(data, "outputFormat", {"PNG", "JPEG", "EXR"});
}

void frameRange(const Json& data) {
    if (integer(data, "frameStart") > integer(data, "frameEnd")) {
        throw InvalidPayload{};
    }
    integer(data, "frameStep", 1);
}

ClientPrefs readPreferences(const Json& data) {
    fields(data,
           {"excludedEngines", "availability", "maxFramesPerJob", "maxConcurrentJobs", "quotaGB"});
    const auto& engines = data.at("excludedEngines");
    if (!engines.is_array()) {
        throw InvalidPayload{};
    }
    std::vector<std::string> excluded;
    for (const auto& value : engines) {
        if (!value.is_string() || (value != "CYCLES" && value != "EEVEE")) {
            throw InvalidPayload{};
        }
        excluded.push_back(value.get<std::string>());
    }
    return {std::move(excluded),
            choice(data, "availability", {"AVAILABLE", "BUSY_GAME", "BUSY_WORK", "DND", "OFFLINE"}),
            optionalInteger(data, "maxFramesPerJob", 1), integer(data, "maxConcurrentJobs", 1),
            optionalInteger(data, "quotaGB", 1)};
}

Message readControl(MessageType type, const Json& data) {
    switch (type) {
    case MessageType::Hello:
        fields(data, {"protocolVersion", "userName", "machineName", "token"});
        // Une version différente reste décodable pour répondre HELLO_ERR en P3.
        return Hello{unsigned32(data, "protocolVersion", UINT32_MAX, 1), text(data, "userName"),
                     text(data, "machineName"), text(data, "token")};
    case MessageType::HelloOk:
        fields(data, {"protocolVersion", "serverName"});
        return HelloOk{unsigned32(data, "protocolVersion", UINT32_MAX, 1),
                       text(data, "serverName")};
    case MessageType::HelloErr:
        fields(data, {"reason"});
        return HelloErr{choice(data, "reason", {"bad_token", "bad_version", "banned"})};
    case MessageType::GpuInfo: {
        fields(data, {"gpuModel", "gpuVendor", "vramMB", "ramMB", "os", "blenderVersion", "backend",
                      "benchmarkScore"});
        std::optional<std::string> version;
        if (!data.at("blenderVersion").is_null()) {
            version = text(data, "blenderVersion");
        }
        return GpuInfo{text(data, "gpuModel"),
                       choice(data, "gpuVendor", {"nvidia", "amd", "intel", "apple", "other"}),
                       integer(data, "vramMB", 0),
                       integer(data, "ramMB", 0),
                       choice(data, "os", {"linux", "windows", "macos"}),
                       std::move(version),
                       choice(data, "backend", {"OPTIX", "CUDA", "HIP", "ONEAPI", "METAL", "CPU"}),
                       optionalInteger(data, "benchmarkScore")};
    }
    case MessageType::ClientPrefs:
        return readPreferences(data);
    case MessageType::StatusUpdate:
        return StatusUpdate{readPreferences(data)};
    case MessageType::Ping:
        fields(data, {});
        return Ping{};
    case MessageType::Pong:
        fields(data, {});
        return Pong{};
    case MessageType::JobOffer:
        fields(data, {"jobId", "projectName", "engine", "frameStart", "frameEnd", "frameStep",
                      "zipBytes", "blenderVersion", "outputFormat"});
        frameRange(data);
        return JobOffer{text(data, "jobId"),
                        text(data, "projectName"),
                        engine(data),
                        integer(data, "frameStart"),
                        integer(data, "frameEnd"),
                        integer(data, "frameStep", 1),
                        unsignedInteger(data, "zipBytes"),
                        text(data, "blenderVersion"),
                        outputFormat(data)};
    case MessageType::JobAccept:
        fields(data, {"jobId"});
        return JobAccept{text(data, "jobId")};
    case MessageType::JobDecline:
        fields(data, {"jobId"});
        return JobDecline{text(data, "jobId")};
    case MessageType::TaskAssign:
        fields(data, {"jobId", "chunkId", "frameStart", "frameEnd", "frameStep", "outputPattern",
                      "outputFormat"});
        frameRange(data);
        return TaskAssign{
            text(data, "jobId"),       text(data, "chunkId"),         integer(data, "frameStart"),
            integer(data, "frameEnd"), integer(data, "frameStep", 1), text(data, "outputPattern"),
            outputFormat(data)};
    case MessageType::Progress: {
        fields(data, {"jobId", "chunkId", "currentFrame", "framesDone", "framesTotal", "percent",
                      "etaSec"});
        const auto& percent = data.at("percent");
        if (!percent.is_number() || !std::isfinite(percent.get<double>()) || percent < 0 ||
            percent > 100) {
            throw InvalidPayload{};
        }
        const int total = integer(data, "framesTotal", 1);
        return Progress{text(data, "jobId"),
                        text(data, "chunkId"),
                        integer(data, "currentFrame"),
                        integer(data, "framesDone", 0, total),
                        total,
                        percent.get<double>(),
                        optionalInteger(data, "etaSec")};
    }
    case MessageType::TaskCancel:
        fields(data, {"jobId", "chunkId"});
        return TaskCancel{text(data, "jobId"), text(data, "chunkId")};
    case MessageType::JobPause:
        fields(data, {"jobId"});
        return JobPause{text(data, "jobId")};
    case MessageType::JobResume:
        fields(data, {"jobId"});
        return JobResume{text(data, "jobId")};
    case MessageType::ClientKick:
        fields(data, {"reason"});
        return ClientKick{text(data, "reason")};
    case MessageType::FileBegin:
        fields(data, {"fileId", "kind", "totalBytes", "sha256", "chunkSize"});
        return FileBegin{fileId(data), choice(data, "kind", {"PROJECT_ZIP", "RESULT_ZIP"}),
                         unsignedInteger(data, "totalBytes"), checksum(data),
                         unsigned32(data, "chunkSize", maxFileBlockBytes, 1)};
    case MessageType::FileEnd:
        fields(data, {"fileId", "sha256"});
        return FileEnd{fileId(data), checksum(data)};
    case MessageType::FileAck:
        fields(data, {"fileId", "nextSeq"});
        return FileAck{fileId(data), unsigned32(data, "nextSeq")};
    case MessageType::FileErr:
        fields(data, {"fileId", "reason"});
        return FileErr{fileId(data),
                       choice(data, "reason", {"sha_mismatch", "disk_full", "aborted"})};
    case MessageType::FileData:
        break;
    }
    throw InvalidPayload{};
}

template <typename T> Json nullable(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json controlJson(const Hello& v) {
    return {{"protocolVersion", v.protocolVersion},
            {"userName", v.userName},
            {"machineName", v.machineName},
            {"token", v.token}};
}
Json controlJson(const HelloOk& v) {
    return {{"protocolVersion", v.protocolVersion}, {"serverName", v.serverName}};
}
Json controlJson(const HelloErr& v) { return {{"reason", v.reason}}; }
Json controlJson(const GpuInfo& v) {
    return {{"gpuModel", v.gpuModel},
            {"gpuVendor", v.gpuVendor},
            {"vramMB", v.vramMB},
            {"ramMB", v.ramMB},
            {"os", v.os},
            {"blenderVersion", nullable(v.blenderVersion)},
            {"backend", v.backend},
            {"benchmarkScore", nullable(v.benchmarkScore)}};
}
Json controlJson(const ClientPrefs& v) {
    return {{"excludedEngines", v.excludedEngines},
            {"availability", v.availability},
            {"maxFramesPerJob", nullable(v.maxFramesPerJob)},
            {"maxConcurrentJobs", v.maxConcurrentJobs},
            {"quotaGB", nullable(v.quotaGB)}};
}
Json controlJson(const Ping&) { return Json::object(); }
Json controlJson(const Pong&) { return Json::object(); }
Json controlJson(const JobOffer& v) {
    return {{"jobId", v.jobId},
            {"projectName", v.projectName},
            {"engine", v.engine},
            {"frameStart", v.frameStart},
            {"frameEnd", v.frameEnd},
            {"frameStep", v.frameStep},
            {"zipBytes", v.zipBytes},
            {"blenderVersion", v.blenderVersion},
            {"outputFormat", v.outputFormat}};
}
Json controlJson(const JobAccept& v) { return {{"jobId", v.jobId}}; }
Json controlJson(const JobDecline& v) { return {{"jobId", v.jobId}}; }
Json controlJson(const TaskAssign& v) {
    return {{"jobId", v.jobId},
            {"chunkId", v.chunkId},
            {"frameStart", v.frameStart},
            {"frameEnd", v.frameEnd},
            {"frameStep", v.frameStep},
            {"outputPattern", v.outputPattern},
            {"outputFormat", v.outputFormat}};
}
Json controlJson(const Progress& v) {
    return {{"jobId", v.jobId},
            {"chunkId", v.chunkId},
            {"currentFrame", v.currentFrame},
            {"framesDone", v.framesDone},
            {"framesTotal", v.framesTotal},
            {"percent", v.percent},
            {"etaSec", nullable(v.etaSec)}};
}
Json controlJson(const TaskCancel& v) { return {{"jobId", v.jobId}, {"chunkId", v.chunkId}}; }
Json controlJson(const JobPause& v) { return {{"jobId", v.jobId}}; }
Json controlJson(const JobResume& v) { return {{"jobId", v.jobId}}; }
Json controlJson(const ClientKick& v) { return {{"reason", v.reason}}; }
Json controlJson(const StatusUpdate& v) { return controlJson(v.preferences); }
Json controlJson(const FileBegin& v) {
    return {{"fileId", v.fileId},
            {"kind", v.kind},
            {"totalBytes", v.totalBytes},
            {"sha256", v.sha256},
            {"chunkSize", v.chunkSize}};
}
Json controlJson(const FileData&) { throw InvalidPayload{}; }
Json controlJson(const FileEnd& v) { return {{"fileId", v.fileId}, {"sha256", v.sha256}}; }
Json controlJson(const FileAck& v) { return {{"fileId", v.fileId}, {"nextSeq", v.nextSeq}}; }
Json controlJson(const FileErr& v) { return {{"fileId", v.fileId}, {"reason", v.reason}}; }

// Le parseur valide aussi l'UTF-8, les doublons de clés et une profondeur bornée.
Json parseJson(std::span<const std::uint8_t> bytes) {
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&objectKeys](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 64) {
            throw InvalidJson{};
        }
        if (event == Json::parse_event_t::object_start) {
            objectKeys.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            objectKeys.pop_back();
        } else if (event == Json::parse_event_t::key &&
                   !objectKeys.back().insert(value.get<std::string>()).second) {
            throw InvalidJson{};
        }
        return true;
    };
    auto data = Json::parse(bytes.begin(), bytes.end(), callback, false);
    if (data.is_discarded()) {
        throw InvalidJson{};
    }
    return data;
}

bool validFileId(const std::string& id) {
    if (id.empty() || id.size() > maxFileIdBytes || id.find('\0') != std::string::npos) {
        return false;
    }
    try {
        // dump() rejette l'UTF-8 invalide sans publier le contenu dans une erreur.
        Json(id).dump();
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

CodecResult<Message> readFileData(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < fileDataPrefixBytes) {
        return failure<Message>(ProtocolError::InvalidFileData);
    }
    const auto idBytes = read32(bytes, 0);
    if (idBytes == 0 || idBytes > maxFileIdBytes || bytes.size() < fileDataPrefixBytes + idBytes) {
        return failure<Message>(ProtocolError::InvalidFileData);
    }
    std::string id(bytes.begin() + 4, bytes.begin() + 4 + idBytes);
    const auto seq = read32(bytes, 4 + idBytes);
    const auto dataBytes = read32(bytes, 8 + idBytes);
    if (!validFileId(id) || dataBytes == 0 || dataBytes > maxFileBlockBytes ||
        bytes.size() - fileDataPrefixBytes - idBytes != dataBytes) {
        return failure<Message>(ProtocolError::InvalidFileData);
    }
    return success<Message>(FileData{
        std::move(id), seq, Bytes(bytes.begin() + fileDataPrefixBytes + idBytes, bytes.end())});
}

} // namespace

std::string_view toString(MessageType type) {
    switch (type) {
    case MessageType::Hello:
        return "HELLO";
    case MessageType::HelloOk:
        return "HELLO_OK";
    case MessageType::HelloErr:
        return "HELLO_ERR";
    case MessageType::GpuInfo:
        return "GPU_INFO";
    case MessageType::ClientPrefs:
        return "CLIENT_PREFS";
    case MessageType::Ping:
        return "PING";
    case MessageType::Pong:
        return "PONG";
    case MessageType::JobOffer:
        return "JOB_OFFER";
    case MessageType::JobAccept:
        return "JOB_ACCEPT";
    case MessageType::JobDecline:
        return "JOB_DECLINE";
    case MessageType::TaskAssign:
        return "TASK_ASSIGN";
    case MessageType::Progress:
        return "PROGRESS";
    case MessageType::TaskCancel:
        return "TASK_CANCEL";
    case MessageType::JobPause:
        return "JOB_PAUSE";
    case MessageType::JobResume:
        return "JOB_RESUME";
    case MessageType::ClientKick:
        return "CLIENT_KICK";
    case MessageType::StatusUpdate:
        return "STATUS_UPDATE";
    case MessageType::FileBegin:
        return "FILE_BEGIN";
    case MessageType::FileData:
        return "FILE_DATA";
    case MessageType::FileEnd:
        return "FILE_END";
    case MessageType::FileAck:
        return "FILE_ACK";
    case MessageType::FileErr:
        return "FILE_ERR";
    }
    return "UNKNOWN";
}

std::string_view toString(ProtocolError error) {
    switch (error) {
    case ProtocolError::None:
        return "ok";
    case ProtocolError::NeedMoreData:
        return "incomplete frame";
    case ProtocolError::InvalidMagic:
        return "invalid protocol magic";
    case ProtocolError::InvalidReserved:
        return "invalid reserved byte";
    case ProtocolError::UnknownMessageType:
        return "unknown message type";
    case ProtocolError::InvalidFlags:
        return "unsupported flags";
    case ProtocolError::PayloadTooLarge:
        return "payload too large";
    case ProtocolError::InvalidJson:
        return "invalid JSON";
    case ProtocolError::InvalidPayload:
        return "invalid message schema or value";
    case ProtocolError::InvalidFileData:
        return "invalid file data block";
    case ProtocolError::TrailingData:
        return "unexpected payload trailing data";
    }
    return "unknown protocol error";
}

MessageType ProtocolCodec::typeOf(const Message& message) {
    return std::visit([](const auto& value) { return std::decay_t<decltype(value)>::type; },
                      message);
}

CodecResult<Bytes> ProtocolCodec::encode(const Message& message) {
    const auto type = typeOf(message);
    Bytes payload;
    try {
        if (const auto* block = std::get_if<FileData>(&message)) {
            if (!validFileId(block->fileId) || block->data.empty() ||
                block->data.size() > maxFileBlockBytes) {
                return failure<Bytes>(ProtocolError::InvalidFileData);
            }
            payload.reserve(fileDataPrefixBytes + block->fileId.size() + block->data.size());
            write32(payload, static_cast<std::uint32_t>(block->fileId.size()));
            payload.insert(payload.end(), block->fileId.begin(), block->fileId.end());
            write32(payload, block->seq);
            write32(payload, static_cast<std::uint32_t>(block->data.size()));
            payload.insert(payload.end(), block->data.begin(), block->data.end());
        } else {
            const auto data =
                std::visit([](const auto& value) { return controlJson(value); }, message);
            // Même validation sémantique dans les deux directions.
            readControl(type, data);
            const auto serialized = data.dump();
            if (serialized.size() > maxControlBytes) {
                return failure<Bytes>(ProtocolError::PayloadTooLarge);
            }
            payload.assign(serialized.begin(), serialized.end());
        }
    } catch (const InvalidPayload&) {
        return failure<Bytes>(ProtocolError::InvalidPayload);
    } catch (const Json::exception&) {
        return failure<Bytes>(ProtocolError::InvalidPayload);
    }

    Bytes result;
    result.reserve(headerBytes + payload.size());
    result.push_back(static_cast<std::uint8_t>(protocolMagic >> 8));
    result.push_back(static_cast<std::uint8_t>(protocolMagic));
    result.push_back(0);
    result.push_back(static_cast<std::uint8_t>(type));
    write32(result, 0);
    write32(result, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return success(std::move(result));
}

CodecResult<FrameHeader> ProtocolCodec::decodeHeader(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < headerBytes) {
        return failure<FrameHeader>(ProtocolError::NeedMoreData);
    }
    if (bytes[0] != (protocolMagic >> 8) || bytes[1] != (protocolMagic & 0xff)) {
        return failure<FrameHeader>(ProtocolError::InvalidMagic);
    }
    if (bytes[2] != 0) {
        return failure<FrameHeader>(ProtocolError::InvalidReserved);
    }
    if (read32(bytes, 4) != 0) {
        return failure<FrameHeader>(ProtocolError::InvalidFlags);
    }
    const FrameHeader header{static_cast<MessageType>(bytes[3]), read32(bytes, 8)};
    const auto error = validateHeader(header);
    return error == ProtocolError::None ? success(header) : failure<FrameHeader>(error);
}

CodecResult<Message> ProtocolCodec::decodePayload(const FrameHeader& header,
                                                  std::span<const std::uint8_t> bytes) {
    const auto error = validateHeader(header);
    if (error != ProtocolError::None) {
        return failure<Message>(error);
    }
    if (bytes.size() < header.payloadBytes) {
        return failure<Message>(ProtocolError::NeedMoreData);
    }
    if (bytes.size() > header.payloadBytes) {
        return failure<Message>(ProtocolError::TrailingData);
    }
    if (header.type == MessageType::FileData) {
        return readFileData(bytes);
    }
    try {
        return success(readControl(header.type, parseJson(bytes)));
    } catch (const InvalidJson&) {
        return failure<Message>(ProtocolError::InvalidJson);
    } catch (const InvalidPayload&) {
        return failure<Message>(ProtocolError::InvalidPayload);
    } catch (const Json::exception&) {
        return failure<Message>(ProtocolError::InvalidPayload);
    }
}

CodecResult<DecodedMessage> ProtocolCodec::decode(std::span<const std::uint8_t> bytes) {
    const auto header = decodeHeader(bytes);
    if (!header) {
        return failure<DecodedMessage>(header.error);
    }
    if (bytes.size() - headerBytes < header.value->payloadBytes) {
        return failure<DecodedMessage>(ProtocolError::NeedMoreData);
    }
    auto payload =
        decodePayload(*header.value, bytes.subspan(headerBytes, header.value->payloadBytes));
    if (!payload) {
        return failure<DecodedMessage>(payload.error);
    }
    return success(
        DecodedMessage{std::move(*payload.value), headerBytes + header.value->payloadBytes});
}

} // namespace blendnet::protocol
