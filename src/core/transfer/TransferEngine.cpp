#include "transfer/TransferEngine.h"
#include "protocol/ProtocolCodec.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace blendnet::transfer {
namespace {
using Json = nlohmann::json;
std::mutex storeMutex;
std::set<std::filesystem::path> leases;
constexpr auto maxSeq = std::numeric_limits<std::uint32_t>::max();
struct StorageFailure {
    TransferError error;
};
std::string canonicalHash(std::string hash) {
    for (auto& value : hash)
        if (value >= 'A' && value <= 'F')
            value = char(value - 'A' + 'a');
    return hash;
}

std::string hex(const unsigned char* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}
struct Digest {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(),
                                                                    EVP_MD_CTX_free};
    bool reset() { return context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1; }
    bool update(const void* data, std::size_t size) {
        return EVP_DigestUpdate(context.get(), data, size) == 1;
    }
    std::optional<std::string> finish() {
        std::array<unsigned char, 32> bytes{};
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1 || size != 32)
            return {};
        return hex(bytes.data(), size);
    }
};
TransferError hashInto(const std::filesystem::path& path, Digest& digest,
                       const std::atomic_bool* cancelled) {
    std::ifstream input(path, std::ios::binary);
    if (!input || !digest.reset())
        return TransferError::IoError;
    std::vector<char> buffer(protocol::maxFileBlockBytes);
    while (input) {
        if (cancelled && cancelled->load())
            return TransferError::Aborted;
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && !digest.update(buffer.data(), static_cast<std::size_t>(count)))
            return TransferError::IoError;
    }
    return input.eof() && !input.bad() ? TransferError::None : TransferError::IoError;
}
std::uint64_t countBlocks(const protocol::FileBegin& offer) {
    return offer.totalBytes == 0 ? 0 : (offer.totalBytes - 1) / offer.chunkSize + 1;
}
std::uint64_t offset(const protocol::FileBegin& offer, std::uint32_t seq) {
    return std::min(offer.totalBytes, std::uint64_t(seq) * offer.chunkSize);
}
bool sameOffer(const protocol::FileBegin& a, const protocol::FileBegin& b) {
    auto normalized = a;
    normalized.sha256 = canonicalHash(normalized.sha256);
    auto other = b;
    other.sha256 = canonicalHash(other.sha256);
    return normalized == other;
}
std::string wireReason(TransferError error) {
    if (error == TransferError::ChecksumMismatch)
        return "sha_mismatch";
    if (error == TransferError::DiskFull)
        return "disk_full";
    return "aborted";
}
bool symlink(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}
Json readJson(const std::filesystem::path& path) {
    if (symlink(path) || std::filesystem::file_size(path) > 8192)
        throw std::runtime_error("Invalid checkpoint");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Unreadable checkpoint");
    return Json::parse(input);
}
protocol::FileBegin readOffer(const Json& data) {
    if (data.at("format") != "blendnet-transfer-v1")
        throw std::runtime_error("Invalid checkpoint");
    const auto& o = data.at("offer");
    // Passer par le codec pour valider aussi les types/ranges des données locales.
    const auto payload = o.dump();
    const auto result = protocol::ProtocolCodec::decodePayload(
        {protocol::MessageType::FileBegin, static_cast<std::uint32_t>(payload.size())},
        std::span(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    if (!result)
        throw std::runtime_error("Invalid checkpoint");
    return std::get<protocol::FileBegin>(*result.value);
}
void atomicJson(const std::filesystem::path& path, const Json& data) {
    auto temporary = path;
    temporary += ".tmp";
    if (symlink(temporary))
        throw std::runtime_error("Unsafe checkpoint");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
        throw StorageFailure{errno == ENOSPC ? TransferError::DiskFull : TransferError::IoError};
#ifndef _WIN32
    std::filesystem::permissions(temporary, std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write);
#endif
    errno = 0;
    output << data.dump() << '\n';
    output.close();
    if (!output)
        throw StorageFailure{errno == ENOSPC ? TransferError::DiskFull : TransferError::IoError};
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Checkpoint rename failed");
#else
    std::filesystem::rename(temporary, path);
#endif
}
std::uint64_t sizeOrZero(const std::filesystem::path& path) {
    return std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
}
std::uint64_t bookedBytes(const std::filesystem::path& directory,
                          const std::filesystem::path& checkpointDirectory) {
    std::uint64_t booked = 0;
    const auto add = [&booked](std::uint64_t value) {
        if (value > UINT64_MAX - booked)
            throw std::runtime_error("Quota overflow");
        booked += value;
    };
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_symlink() || !entry.is_regular_file())
            continue;
        const auto path = entry.path();
        const auto stem = path.stem().string();
        const bool checkpoint = path.parent_path() == checkpointDirectory && stem.size() == 64 &&
                                std::all_of(stem.begin(), stem.end(), [](char c) {
                                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                });
        if (path.extension() == ".json" && checkpoint) {
            const auto offer = readOffer(readJson(path));
            auto part = path;
            part.replace_extension(".part");
            auto complete = path;
            complete.replace_extension(".zip");
            const auto partBytes = sizeOrZero(part);
            const auto finalBytes = sizeOrZero(complete);
            if (partBytes > UINT64_MAX - finalBytes)
                throw std::runtime_error("Quota overflow");
            add(std::max(offer.totalBytes, partBytes + finalBytes));
        } else if (checkpoint && (path.extension() == ".part" || path.extension() == ".zip")) {
            auto metadata = path;
            metadata.replace_extension(".json");
            if (!std::filesystem::exists(metadata))
                add(entry.file_size());
        } else
            add(entry.file_size());
    }
    return booked;
}
std::uint64_t presentBytes(const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_symlink() && entry.is_regular_file() && entry.path().extension() != ".json") {
            const auto size = entry.file_size();
            if (size > UINT64_MAX - bytes)
                throw std::runtime_error("Size overflow");
            bytes += size;
        }
    }
    return bytes;
}
} // namespace

std::string_view toString(TransferError error) {
    switch (error) {
    case TransferError::None:
        return "ok";
    case TransferError::InvalidRequest:
        return "invalid transfer request";
    case TransferError::IoError:
        return "transfer disk I/O failed";
    case TransferError::DiskFull:
        return "transfer disk quota or space exceeded";
    case TransferError::ChecksumMismatch:
        return "transfer SHA-256 mismatch";
    case TransferError::InvalidSequence:
        return "invalid transfer sequence or block size";
    case TransferError::ConflictingOffer:
        return "transfer metadata conflict";
    case TransferError::CorruptState:
        return "invalid transfer checkpoint";
    case TransferError::Busy:
        return "transfer already active";
    case TransferError::Aborted:
        return "transfer aborted";
    }
    return "unknown transfer error";
}
std::string storageKey(std::string_view text) {
    std::array<unsigned char, 32> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != 32)
        return {};
    return hex(bytes.data(), size);
}
TransferResult<std::string> sha256File(const std::filesystem::path& path,
                                       const std::atomic_bool* cancelled) {
    Digest digest;
    const auto error = hashInto(path, digest, cancelled);
    if (error != TransferError::None)
        return {{}, error};
    const auto hash = digest.finish();
    return hash ? TransferResult<std::string>{*hash, TransferError::None}
                : TransferResult<std::string>{{}, TransferError::IoError};
}
bool isFileMessage(const protocol::Message& message) {
    return static_cast<unsigned int>(protocol::ProtocolCodec::typeOf(message)) >= 30;
}
bool removeCompletedTransfer(const std::filesystem::path& path, const std::string& fileId) {
    try {
        const std::lock_guard lock(storeMutex);
        auto metadata = path;
        metadata.replace_extension(".json");
        if (path.extension() != ".zip" || symlink(path) || symlink(metadata) ||
            leases.contains(metadata))
            return false;
        const auto state = readJson(metadata);
        const auto offer = readOffer(state);
        if (state.at("complete") != true || offer.fileId != fileId ||
            std::filesystem::file_size(path) != offer.totalBytes)
            return false;
        std::filesystem::remove(path);
        std::filesystem::remove(metadata);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct TransferReceiver::Impl {
    TransferOptions options;
    std::string scope;
    protocol::FileBegin offer;
    std::uint32_t nextSeq = 0;
    std::filesystem::path part, final, metadata;
    std::ofstream output;
    Digest digest;
    TransferProgress state;
    bool leased = false;
    bool active = false;

    Impl(TransferOptions settings, std::string peer)
        : options(std::move(settings)), scope(std::move(peer)) {}
    ~Impl() { suspend(); }
    void release() {
        if (output.is_open())
            output.close();
        if (leased) {
            const std::lock_guard lock(storeMutex);
            leases.erase(metadata);
            leased = false;
        }
        active = false;
    }
    void discard() {
        if (output.is_open())
            output.close();
        const std::lock_guard lock(storeMutex);
        // Garder le lease jusqu'à la suppression pour qu'un nouvel écrivain ne soit
        // jamais installé entre notre fermeture et le nettoyage du partiel invalide.
        std::filesystem::remove(part);
        std::filesystem::remove(metadata);
        if (leased)
            leases.erase(metadata);
        leased = active = false;
    }
    void checkpoint(bool complete = false) {
        if (output.is_open()) {
            errno = 0;
            output.flush();
            if (!output)
                throw StorageFailure{errno == ENOSPC ? TransferError::DiskFull
                                                     : TransferError::IoError};
        }
        const Json data{{"format", "blendnet-transfer-v1"},
                        {"nextSeq", nextSeq},
                        {"complete", complete},
                        {"offer",
                         {{"fileId", offer.fileId},
                          {"kind", offer.kind},
                          {"totalBytes", offer.totalBytes},
                          {"sha256", offer.sha256},
                          {"chunkSize", offer.chunkSize}}}};
        atomicJson(metadata, data);
    }
    std::optional<protocol::Message> fail(TransferError error, const std::string& id = {}) {
        state.state = TransferState::Failed;
        state.error = error;
        const auto identifier = id.empty() ? offer.fileId : id;
        release();
        return protocol::FileErr{identifier, wireReason(error)};
    }
    void suspend() {
        if (!active)
            return;
        try {
            checkpoint();
            state.state = TransferState::Suspended;
        } catch (const StorageFailure& failure) {
            state.state = TransferState::Failed;
            state.error = failure.error;
        } catch (const std::exception&) {
            state.state = TransferState::Failed;
            state.error = TransferError::IoError;
        }
        release();
    }
    std::optional<protocol::Message> begin(const protocol::FileBegin& incoming,
                                           const std::atomic_bool* cancelled) {
        if (!protocol::ProtocolCodec::encode(incoming) || countBlocks(incoming) > maxSeq ||
            options.directory.empty())
            return fail(TransferError::InvalidRequest, incoming.fileId);
        if (active && !sameOffer(offer, incoming))
            return protocol::FileErr{incoming.fileId, "aborted"};
        suspend();
        offer = incoming;
        offer.sha256 = canonicalHash(offer.sha256);
        state = {offer.fileId, Direction::Receiving, TransferState::Preparing, TransferError::None,
                 offer.totalBytes};
        std::filesystem::create_directories(options.directory);
        const auto key = storageKey(scope + std::string(1, '\0') + offer.fileId);
        if (key.empty())
            return fail(TransferError::IoError);
        metadata = std::filesystem::weakly_canonical(options.directory) / (key + ".json");
        part = metadata;
        part.replace_extension(".part");
        final = metadata;
        final.replace_extension(".zip");
        if (symlink(part) || symlink(final) || symlink(metadata))
            return fail(TransferError::CorruptState);
        nextSeq = 0;
        {
            const std::lock_guard lock(storeMutex);
            if (leases.contains(metadata))
                return protocol::FileErr{offer.fileId, "aborted"};
            const bool existing = std::filesystem::exists(metadata);
            if (existing) {
                const auto saved = readJson(metadata);
                if (!sameOffer(readOffer(saved), offer))
                    return protocol::FileErr{offer.fileId, "aborted"};
                const auto& seq = saved.at("nextSeq");
                if (!seq.is_number_integer() || seq < 0 || seq > countBlocks(offer))
                    return protocol::FileErr{offer.fileId, "aborted"};
                nextSeq = seq.get<std::uint32_t>();
                if (!saved.at("complete").is_boolean())
                    return protocol::FileErr{offer.fileId, "aborted"};
            } else if (std::filesystem::exists(part) || std::filesystem::exists(final)) {
                return protocol::FileErr{offer.fileId, "aborted"};
            }
            const auto quotaRoot =
                options.quotaDirectory.empty() ? options.directory : options.quotaDirectory;
            const auto booked = bookedBytes(quotaRoot, options.directory);
            const auto additional = existing ? 0 : offer.totalBytes;
            if (options.quotaBytes &&
                (booked > *options.quotaBytes || additional > *options.quotaBytes - booked))
                return protocol::FileErr{offer.fileId, "disk_full"};
            const auto used = presentBytes(quotaRoot);
            const auto reserved = booked - std::min(booked, used);
            const auto available = std::filesystem::space(options.directory).available;
            if (additional > available || reserved > available - additional)
                return protocol::FileErr{offer.fileId, "disk_full"};
            if (!existing)
                checkpoint();
            leases.insert(metadata);
            leased = true;
        }
        if (std::filesystem::exists(final)) {
            const auto hash = sha256File(final, cancelled);
            if (!hash || sizeOrZero(final) != offer.totalBytes || *hash.value != offer.sha256)
                return fail(hash.error == TransferError::Aborted ? hash.error
                                                                 : TransferError::ChecksumMismatch);
            nextSeq = static_cast<std::uint32_t>(countBlocks(offer));
            checkpoint(true);
            state.completedBytes = state.resumedBytes = offer.totalBytes;
            state.path = final;
            state.state = TransferState::Completed;
            release();
            return protocol::FileAck{offer.fileId, nextSeq};
        }
        const auto committed = offset(offer, nextSeq);
        if (committed > sizeOrZero(part))
            return fail(TransferError::CorruptState);
        if (!std::filesystem::exists(part)) {
            std::ofstream create(part, std::ios::binary);
            if (!create)
                return fail(TransferError::IoError);
#ifndef _WIN32
            std::filesystem::permissions(part, std::filesystem::perms::owner_read |
                                                   std::filesystem::perms::owner_write);
#endif
        }
        std::filesystem::resize_file(part, committed);
        const auto hashed = hashInto(part, digest, cancelled);
        if (hashed != TransferError::None)
            return fail(hashed);
        output.open(part, std::ios::binary | std::ios::app);
        if (!output)
            return fail(TransferError::IoError);
        active = true;
        state.state = TransferState::Transferring;
        state.completedBytes = state.resumedBytes = committed;
        state.path = part;
        return protocol::FileAck{offer.fileId, nextSeq};
    }
    std::optional<protocol::Message> data(const protocol::FileData& block) {
        if (!active || block.fileId != offer.fileId)
            return protocol::FileErr{block.fileId, "aborted"};
        const auto position = offset(offer, nextSeq);
        const auto expected = std::min<std::uint64_t>(offer.chunkSize, offer.totalBytes - position);
        if (block.seq != nextSeq || expected == 0 || block.data.size() != expected)
            return fail(TransferError::InvalidSequence);
        errno = 0;
        output.write(reinterpret_cast<const char*>(block.data.data()),
                     static_cast<std::streamsize>(block.data.size()));
        if (!output)
            return fail(errno == ENOSPC ? TransferError::DiskFull : TransferError::IoError);
        if (!digest.update(block.data.data(), block.data.size()))
            return fail(TransferError::IoError);
        ++nextSeq;
        state.completedBytes = offset(offer, nextSeq);
        if (nextSeq % ackBatchBlocks == 0 || state.completedBytes == offer.totalBytes) {
            checkpoint();
            return protocol::FileAck{offer.fileId, nextSeq};
        }
        return {};
    }
    std::optional<protocol::Message> end(const protocol::FileEnd& ending) {
        if (ending.fileId != offer.fileId)
            return protocol::FileErr{ending.fileId, "aborted"};
        if (state.state == TransferState::Completed && canonicalHash(ending.sha256) == offer.sha256)
            return protocol::FileAck{offer.fileId, static_cast<std::uint32_t>(countBlocks(offer))};
        if (!active || nextSeq != countBlocks(offer))
            return fail(TransferError::InvalidSequence);
        const auto hash = digest.finish();
        if (!hash || *hash != offer.sha256 || canonicalHash(ending.sha256) != offer.sha256) {
            // Uniquement nos fichiers partiels, jamais un résultat déjà validé.
            discard();
            return fail(TransferError::ChecksumMismatch);
        }
        output.close();
        if (!output)
            return fail(TransferError::IoError);
        if (std::filesystem::exists(final))
            return fail(TransferError::ConflictingOffer);
        std::filesystem::rename(part, final);
        checkpoint(true);
        state.state = TransferState::Completed;
        state.completedBytes = offer.totalBytes;
        state.path = final;
        release();
        return protocol::FileAck{offer.fileId, nextSeq};
    }
};
TransferReceiver::TransferReceiver(TransferOptions options, std::string peerScope)
    : m_impl(std::make_unique<Impl>(std::move(options), std::move(peerScope))) {}
TransferReceiver::~TransferReceiver() = default;
std::optional<protocol::Message> TransferReceiver::receive(const protocol::Message& message,
                                                           const std::atomic_bool* cancelled) {
    try {
        if (const auto* begin = std::get_if<protocol::FileBegin>(&message))
            return m_impl->begin(*begin, cancelled);
        if (const auto* data = std::get_if<protocol::FileData>(&message))
            return m_impl->data(*data);
        if (const auto* end = std::get_if<protocol::FileEnd>(&message))
            return m_impl->end(*end);
        if (const auto* error = std::get_if<protocol::FileErr>(&message)) {
            if (error->fileId == m_impl->offer.fileId && m_impl->active) {
                m_impl->state.state = TransferState::Failed;
                m_impl->state.error = TransferError::Aborted;
                // FILE_ERR est une annulation explicite ; une coupure réseau utilise suspend().
                m_impl->discard();
            }
        }
        return {};
    } catch (const StorageFailure& failure) {
        return m_impl->fail(failure.error);
    } catch (const std::filesystem::filesystem_error& error) {
        return m_impl->fail(error.code() == std::errc::no_space_on_device ? TransferError::DiskFull
                                                                          : TransferError::IoError);
    } catch (const std::exception&) {
        return m_impl->fail(TransferError::IoError);
    }
}
void TransferReceiver::suspend() { m_impl->suspend(); }
TransferProgress TransferReceiver::progress() const { return m_impl->state; }

struct TransferSender::Impl {
    enum class Stage { Initial, BeginAck, Sending, FinalAck, Done, Failed };
    Stage stage = Stage::Initial;
    protocol::FileBegin offer;
    std::ifstream input;
    std::uint32_t nextSeq = 0, acknowledged = 0;
    TransferProgress state;
};
TransferSender::TransferSender() : m_impl(std::make_unique<Impl>()) {}
TransferSender::~TransferSender() = default;
TransferResult<protocol::FileBegin> TransferSender::prepare(const FileRequest& request,
                                                            const std::atomic_bool* cancelled) {
    auto& s = *m_impl;
    s.input.close();
    s.input.clear();
    s.state = {
        request.fileId, Direction::Sending, TransferState::Preparing, TransferError::None, 0, 0, 0,
        request.path};
    try {
        if (!std::filesystem::is_regular_file(request.path)) {
            fail(TransferError::InvalidRequest);
            return {{}, s.state.error};
        }
        const auto size = std::filesystem::file_size(request.path);
        s.offer = {request.fileId, request.kind, size, std::string(64, '0'), request.chunkSize};
        if (!protocol::ProtocolCodec::encode(s.offer) || countBlocks(s.offer) > maxSeq) {
            fail(TransferError::InvalidRequest);
            return {{}, s.state.error};
        }
        const auto hash = sha256File(request.path, cancelled);
        if (!hash) {
            fail(hash.error);
            return {{}, hash.error};
        }
        s.offer.sha256 = *hash.value;
        s.input.open(request.path, std::ios::binary);
        if (!s.input) {
            fail(TransferError::IoError);
            return {{}, s.state.error};
        }
        s.state.totalBytes = size;
        s.state.state = TransferState::Transferring;
        s.nextSeq = s.acknowledged = 0;
        s.stage = Impl::Stage::BeginAck;
        return {s.offer, TransferError::None};
    } catch (const std::exception&) {
        fail(TransferError::IoError);
        return {{}, s.state.error};
    }
}
TransferError TransferSender::acknowledge(const protocol::FileAck& ack) {
    auto& s = *m_impl;
    if (s.stage != Impl::Stage::BeginAck && s.stage != Impl::Stage::Sending &&
        s.stage != Impl::Stage::FinalAck) {
        fail(TransferError::InvalidSequence);
        return s.state.error;
    }
    const auto count = countBlocks(s.offer);
    if (ack.fileId != s.offer.fileId || ack.nextSeq > count ||
        (s.stage != Impl::Stage::BeginAck &&
         (ack.nextSeq < s.acknowledged || ack.nextSeq > s.nextSeq))) {
        fail(TransferError::InvalidSequence);
        return s.state.error;
    }
    if (s.stage == Impl::Stage::BeginAck) {
        s.nextSeq = s.acknowledged = ack.nextSeq;
        const auto position = offset(s.offer, ack.nextSeq);
        s.input.clear();
        s.input.seekg(static_cast<std::streamoff>(position));
        if (!s.input) {
            fail(TransferError::IoError);
            return s.state.error;
        }
        s.state.resumedBytes = position;
        s.stage = Impl::Stage::Sending;
    } else if (s.stage == Impl::Stage::FinalAck) {
        if (ack.nextSeq != count) {
            fail(TransferError::InvalidSequence);
            return s.state.error;
        }
        s.state.state = TransferState::Completed;
        s.stage = Impl::Stage::Done;
        s.input.close();
    } else if (s.stage != Impl::Stage::Sending) {
        fail(TransferError::InvalidSequence);
        return s.state.error;
    }
    s.acknowledged = ack.nextSeq;
    s.state.completedBytes = offset(s.offer, ack.nextSeq);
    return TransferError::None;
}
std::optional<protocol::Message> TransferSender::next() {
    auto& s = *m_impl;
    if (s.stage != Impl::Stage::Sending)
        return {};
    if (s.acknowledged == countBlocks(s.offer)) {
        s.stage = Impl::Stage::FinalAck;
        return protocol::FileEnd{s.offer.fileId, s.offer.sha256};
    }
    if (s.nextSeq == countBlocks(s.offer) || s.nextSeq - s.acknowledged >= windowBlocks)
        return {};
    const auto bytes =
        std::min<std::uint64_t>(s.offer.chunkSize, s.offer.totalBytes - offset(s.offer, s.nextSeq));
    protocol::Bytes buffer(static_cast<std::size_t>(bytes));
    s.input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytes));
    if (s.input.gcount() != static_cast<std::streamsize>(bytes) || s.input.bad()) {
        fail(TransferError::IoError);
        return {};
    }
    return protocol::FileData{s.offer.fileId, s.nextSeq++, std::move(buffer)};
}
void TransferSender::fail(TransferError error) {
    m_impl->state.error = error;
    m_impl->state.state = TransferState::Failed;
    m_impl->stage = Impl::Stage::Failed;
    m_impl->input.close();
}
TransferProgress TransferSender::progress() const { return m_impl->state; }
} // namespace blendnet::transfer
