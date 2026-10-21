#include "protocol/ProtocolCodec.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>

using namespace blendnet::protocol;

namespace {

// Fixtures écrites indépendamment du codec, y compris le framing réseau.
void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (int index = 0; index < 4; ++index) {
        bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (24 - 8 * index));
    }
}

Bytes frame(MessageType type, std::string_view payload) {
    Bytes result{0x52, 0x53, 0, static_cast<std::uint8_t>(type), 0, 0, 0, 0, 0, 0, 0, 0};
    put32(result, 8, static_cast<std::uint32_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

struct Fixture {
    Message message;
    std::string json;
};

std::vector<Fixture> controls() {
    const std::string hash(64, 'a');
    const ClientPrefs preferences{{"EEVEE"}, "AVAILABLE", 200, 2, 100};
    return {
        {Hello{1, "Alice", "render-pc", "secret-token"},
         R"({"protocolVersion":1,"userName":"Alice","machineName":"render-pc","token":"secret-token"})"},
        {HelloOk{1, "source"}, R"({"protocolVersion":1,"serverName":"source"})"},
        {HelloErr{"bad_token"}, R"({"reason":"bad_token"})"},
        {GpuInfo{"RTX", "nvidia", 8192, 32768, "linux", "4.2.1", "OPTIX", 125},
         R"({"gpuModel":"RTX","gpuVendor":"nvidia","vramMB":8192,"ramMB":32768,"os":"linux","blenderVersion":"4.2.1","backend":"OPTIX","benchmarkScore":125})"},
        {preferences,
         R"({"excludedEngines":["EEVEE"],"availability":"AVAILABLE","maxFramesPerJob":200,"maxConcurrentJobs":2,"quotaGB":100})"},
        {Ping{}, "{}"},
        {Pong{}, "{}"},
        {JobOffer{"job", "Scène été", "CYCLES", -2, 100, 2, 10737418240ULL, "4.2.1", "EXR"},
         R"({"jobId":"job","projectName":"Scène été","engine":"CYCLES","frameStart":-2,"frameEnd":100,"frameStep":2,"zipBytes":10737418240,"blenderVersion":"4.2.1","outputFormat":"EXR"})"},
        {JobAccept{"job"}, R"({"jobId":"job"})"},
        {JobDecline{"job"}, R"({"jobId":"job"})"},
        {TaskAssign{"job", "job_c0", 1, 20, 1, "output/frame_####", "PNG"},
         R"({"jobId":"job","chunkId":"job_c0","frameStart":1,"frameEnd":20,"frameStep":1,"outputPattern":"output/frame_####","outputFormat":"PNG"})"},
        {Progress{"job", "job_c0", 10, 10, 20, 50.5, 30},
         R"({"jobId":"job","chunkId":"job_c0","currentFrame":10,"framesDone":10,"framesTotal":20,"percent":50.5,"etaSec":30})"},
        {TaskCancel{"job", "job_c0"}, R"({"jobId":"job","chunkId":"job_c0"})"},
        {JobPause{"job"}, R"({"jobId":"job"})"},
        {JobResume{"job"}, R"({"jobId":"job"})"},
        {ClientKick{"user requested"}, R"({"reason":"user requested"})"},
        {StatusUpdate{preferences},
         R"({"excludedEngines":["EEVEE"],"availability":"AVAILABLE","maxFramesPerJob":200,"maxConcurrentJobs":2,"quotaGB":100})"},
        {FileBegin{"job_PROJECT_ZIP", "PROJECT_ZIP", 10737418240ULL, hash, 1048576},
         R"({"fileId":"job_PROJECT_ZIP","kind":"PROJECT_ZIP","totalBytes":10737418240,"sha256":")" +
             hash + R"(","chunkSize":1048576})"},
        {FileEnd{"job_RESULT_ZIP", hash},
         R"({"fileId":"job_RESULT_ZIP","sha256":")" + hash + R"("})"},
        {FileAck{"job_RESULT_ZIP", 0x01020304},
         R"({"fileId":"job_RESULT_ZIP","nextSeq":16909060})"},
        {FileErr{"job_PROJECT_ZIP", "sha_mismatch"},
         R"({"fileId":"job_PROJECT_ZIP","reason":"sha_mismatch"})"}};
}

} // namespace

TEST(ProtocolCodecTest, IndependentControlFixturesAndEveryMessageTypeRoundTrip) {
    std::set<MessageType> types;
    for (const auto& fixture : controls()) {
        const auto type = ProtocolCodec::typeOf(fixture.message);
        SCOPED_TRACE(toString(type));
        types.insert(type);
        const auto wire = frame(type, fixture.json);
        const auto decoded = ProtocolCodec::decode(wire);
        ASSERT_TRUE(decoded) << toString(decoded.error);
        EXPECT_TRUE(decoded.value->message == fixture.message);
        EXPECT_EQ(decoded.value->bytesConsumed, wire.size());

        const auto encoded = ProtocolCodec::encode(fixture.message);
        ASSERT_TRUE(encoded) << toString(encoded.error);
        const auto header = ProtocolCodec::decodeHeader(*encoded.value);
        ASSERT_TRUE(header);
        EXPECT_EQ(header.value->type, type);
        EXPECT_EQ(header.value->payloadBytes + 12, encoded.value->size());
        const auto roundTrip = ProtocolCodec::decode(*encoded.value);
        ASSERT_TRUE(roundTrip);
        EXPECT_TRUE(roundTrip.value->message == fixture.message);
    }
    EXPECT_EQ(types.size(), std::variant_size_v<Message> - 1); // FILE_DATA est binaire.
}

TEST(ProtocolCodecTest, ExactNetworkByteOrderAndTwelveByteHeader) {
    const Bytes expected{0x52, 0x53, 0, 5, 0, 0, 0, 0, 0, 0, 0, 2, '{', '}'};
    const auto encoded = ProtocolCodec::encode(Ping{});
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded.value, expected);
    const auto decoded = ProtocolCodec::decode(expected);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(std::holds_alternative<Ping>(decoded.value->message));

    Bytes header{0x52, 0x53, 0, 31, 0, 0, 0, 0, 0, 1, 2, 3};
    const auto parsed = ProtocolCodec::decodeHeader(header);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.value->payloadBytes, 0x010203U);
}

TEST(ProtocolCodecTest, BinaryFileDataUsesTheSameTextualIdAndPreservesAllBytes) {
    const FileData block{"job_PROJECT_ZIP", 0x01020304, {0, 0xff, 0x80, 'R', 'S', '\n'}};
    Bytes expected{0x52, 0x53, 0,   31,  0,   0,   0,   0,   0,   0,   0,    33,   0,   0,   0,
                   15,   'j',  'o', 'b', '_', 'P', 'R', 'O', 'J', 'E', 'C',  'T',  '_', 'Z', 'I',
                   'P',  1,    2,   3,   4,   0,   0,   0,   6,   0,   0xff, 0x80, 'R', 'S', '\n'};
    const auto encoded = ProtocolCodec::encode(block);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded.value, expected);
    const auto decoded = ProtocolCodec::decode(expected);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<FileData>(decoded.value->message), block);

    const FileData unicode{"scène_RESULT_ZIP", UINT32_MAX, {1}};
    const auto utf8 = ProtocolCodec::encode(unicode);
    ASSERT_TRUE(utf8);
    const auto result = ProtocolCodec::decode(*utf8.value);
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<FileData>(result.value->message), unicode);
}

TEST(ProtocolCodecTest, TruncationAtEveryByteWaitsWithoutProducingAMessage) {
    for (const Message& message : {Message(Hello{1, "Alice", "PC", "token"}),
                                   Message(FileData{"job_PROJECT_ZIP", 0, {1, 2, 3}})}) {
        const auto encoded = ProtocolCodec::encode(message);
        ASSERT_TRUE(encoded);
        for (std::size_t size = 0; size < encoded.value->size(); ++size) {
            SCOPED_TRACE(size);
            const auto result = ProtocolCodec::decode(std::span(*encoded.value).first(size));
            EXPECT_FALSE(result);
            EXPECT_EQ(result.error, ProtocolError::NeedMoreData);
            EXPECT_FALSE(result.value.has_value());
        }
    }
}

TEST(ProtocolCodecTest, ConcatenatedFramesReportExactlyTheConsumedPrefix) {
    auto wire = frame(MessageType::Ping, "{}");
    const auto firstSize = wire.size();
    const auto second = frame(MessageType::Pong, "{}");
    wire.insert(wire.end(), second.begin(), second.end());
    wire.insert(wire.end(), {0x52, 0x53});
    const auto first = ProtocolCodec::decode(wire);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value->bytesConsumed, firstSize);
    const auto next = ProtocolCodec::decode(std::span(wire).subspan(firstSize));
    ASSERT_TRUE(next);
    EXPECT_TRUE(std::holds_alternative<Pong>(next.value->message));
    EXPECT_EQ(next.value->bytesConsumed, second.size());
    const auto tail = ProtocolCodec::decode(std::span(wire).subspan(firstSize + second.size()));
    EXPECT_EQ(tail.error, ProtocolError::NeedMoreData);
}

TEST(ProtocolCodecTest, InvalidHeadersFailBeforeReceivingThePayload) {
    const auto original = frame(MessageType::Hello, "{}");
    for (const auto& [offset, value, error] :
         {std::tuple<std::size_t, std::uint8_t, ProtocolError>{0, 0, ProtocolError::InvalidMagic},
          {1, 0, ProtocolError::InvalidMagic},
          {2, 1, ProtocolError::InvalidReserved},
          {3, 255, ProtocolError::UnknownMessageType},
          {4, 1, ProtocolError::InvalidFlags},
          {7, 1, ProtocolError::InvalidFlags}}) {
        auto wire = original;
        wire[offset] = value;
        const auto result = ProtocolCodec::decode(wire);
        EXPECT_EQ(result.error, error);
        EXPECT_FALSE(result);
    }
    for (const auto type : {MessageType::Hello, MessageType::FileData}) {
        auto wire = frame(type, "");
        put32(wire, 8, UINT32_MAX);
        EXPECT_EQ(ProtocolCodec::decodeHeader(wire).error, ProtocolError::PayloadTooLarge);
        EXPECT_EQ(ProtocolCodec::decode(wire).error, ProtocolError::PayloadTooLarge);
    }
}

TEST(ProtocolCodecTest, ControlAndBinaryLimitsAreInclusiveAndCheckedOnBothPaths) {
    // {"reason":"..."} ajoute exactement 13 octets.
    const auto boundary = ProtocolCodec::encode(ClientKick{std::string(maxControlBytes - 13, 'a')});
    ASSERT_TRUE(boundary);
    EXPECT_EQ(boundary.value->size(), 12 + maxControlBytes);
    EXPECT_TRUE(ProtocolCodec::decode(*boundary.value));
    const auto oversized =
        ProtocolCodec::encode(ClientKick{std::string(maxControlBytes - 12, 'a')});
    EXPECT_EQ(oversized.error, ProtocolError::PayloadTooLarge);

    FileData block{std::string(maxFileIdBytes, 'i'), 0, Bytes(maxFileBlockBytes, 0xff)};
    const auto maximum = ProtocolCodec::encode(block);
    ASSERT_TRUE(maximum);
    EXPECT_EQ(maximum.value->size(), 12 + maxFileDataBytes);
    const auto decoded = ProtocolCodec::decode(*maximum.value);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<FileData>(decoded.value->message), block);
    block.data.push_back(0);
    EXPECT_EQ(ProtocolCodec::encode(block).error, ProtocolError::InvalidFileData);

    EXPECT_EQ(ProtocolCodec::decodePayload({MessageType::Hello, maxControlBytes + 1}, {}).error,
              ProtocolError::PayloadTooLarge);
    EXPECT_EQ(ProtocolCodec::decodePayload({static_cast<MessageType>(255), 0}, {}).error,
              ProtocolError::UnknownMessageType);
    const Bytes emptyObject{'{', '}'};
    EXPECT_EQ(ProtocolCodec::decodePayload({MessageType::Ping, 3}, emptyObject).error,
              ProtocolError::NeedMoreData);
    EXPECT_EQ(ProtocolCodec::decodePayload({MessageType::Ping, 1}, emptyObject).error,
              ProtocolError::TrailingData);
}

TEST(ProtocolCodecTest, InvalidJsonIsRejectedForEveryControlTypeWithoutLeakingTokens) {
    const std::vector<std::string> malformed{"",
                                             "{",
                                             R"({"token":"secret-token",})",
                                             "{}{}",
                                             "{\"x\":\"\xff\"}",
                                             R"({"jobId":"a","jobId":"b"})",
                                             std::string(100, '[') + "0" + std::string(100, ']')};
    for (const auto& fixture : controls()) {
        for (const auto& payload : malformed) {
            const auto result =
                ProtocolCodec::decode(frame(ProtocolCodec::typeOf(fixture.message), payload));
            EXPECT_FALSE(result);
            EXPECT_EQ(result.error, ProtocolError::InvalidJson);
            EXPECT_EQ(toString(result.error).find("secret-token"), std::string_view::npos);
        }
        const auto result =
            ProtocolCodec::decode(frame(ProtocolCodec::typeOf(fixture.message), "[]"));
        EXPECT_EQ(result.error, ProtocolError::InvalidPayload);
    }
}

TEST(ProtocolCodecTest, EveryControlFieldIsRequiredAndRejectsAnObjectInPlaceOfItsValue) {
    for (const auto& fixture : controls()) {
        const auto type = ProtocolCodec::typeOf(fixture.message);
        const auto source = nlohmann::json::parse(fixture.json);
        SCOPED_TRACE(toString(type));
        for (const auto& [key, value] : source.items()) {
            SCOPED_TRACE(key);
            auto missing = source;
            missing.erase(key);
            EXPECT_EQ(ProtocolCodec::decode(frame(type, missing.dump())).error,
                      ProtocolError::InvalidPayload);
            auto wrongType = source;
            wrongType[key] = nlohmann::json::object();
            EXPECT_EQ(ProtocolCodec::decode(frame(type, wrongType.dump())).error,
                      ProtocolError::InvalidPayload);
        }
        auto extra = source;
        extra["unknownField"] = 1;
        EXPECT_EQ(ProtocolCodec::decode(frame(type, extra.dump())).error,
                  ProtocolError::InvalidPayload);
    }
}

TEST(ProtocolCodecTest, RequiredFieldsTypesEnumsAndRangesAreValidated) {
    const std::vector<std::pair<MessageType, std::string>> invalid{
        {MessageType::Hello, R"({"protocolVersion":1,"userName":"Alice","machineName":"PC"})"},
        {MessageType::Hello,
         R"({"protocolVersion":true,"userName":"Alice","machineName":"PC","token":"x"})"},
        {MessageType::Hello,
         R"({"protocolVersion":4294967296,"userName":"Alice","machineName":"PC","token":"x"})"},
        {MessageType::Hello,
         R"({"protocolVersion":1,"userName":"Alice","machineName":"PC","token":null})"},
        {MessageType::HelloErr, R"({"reason":"unexpected"})"},
        {MessageType::JobAccept, R"({"jobId":""})"},
        {MessageType::JobAccept, R"({"jobId":"job\u0000other"})"},
        {MessageType::Ping, R"({"extra":1})"},
        {MessageType::FileAck, R"({"fileId":"job","nextSeq":-1})"},
        {MessageType::FileAck, R"({"fileId":"job","nextSeq":1.0})"},
        {MessageType::FileAck, R"({"fileId":"job","nextSeq":4294967296})"},
        {MessageType::FileErr, R"({"fileId":"job","reason":"bad"})"},
        {MessageType::FileEnd, R"({"fileId":"job","sha256":"xyz"})"},
        {MessageType::ClientPrefs,
         R"({"excludedEngines":["OTHER"],"availability":"AVAILABLE","maxFramesPerJob":null,"maxConcurrentJobs":1,"quotaGB":null})"},
        {MessageType::StatusUpdate,
         R"({"excludedEngines":[],"availability":"invalid","maxFramesPerJob":null,"maxConcurrentJobs":1,"quotaGB":null})"},
        {MessageType::Progress,
         R"({"jobId":"job","chunkId":"c","currentFrame":1,"framesDone":2,"framesTotal":1,"percent":100,"etaSec":null})"},
        {MessageType::Progress,
         R"({"jobId":"job","chunkId":"c","currentFrame":1,"framesDone":0,"framesTotal":1,"percent":101,"etaSec":null})"},
        {MessageType::Progress,
         R"({"jobId":"job","chunkId":"c","currentFrame":1,"framesDone":0,"framesTotal":1,"percent":0,"etaSec":-1})"}};
    for (const auto& [type, payload] : invalid) {
        SCOPED_TRACE(toString(type));
        SCOPED_TRACE(payload);
        const auto result = ProtocolCodec::decode(frame(type, payload));
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error, ProtocolError::InvalidPayload);
    }

    EXPECT_EQ(ProtocolCodec::encode(Hello{1, "Alice", "PC", ""}).error,
              ProtocolError::InvalidPayload);
    EXPECT_EQ(ProtocolCodec::encode(JobOffer{"job", "scene", "CYCLES", 20, 1, 1, 0, "4.2.1", "PNG"})
                  .error,
              ProtocolError::InvalidPayload);
    EXPECT_EQ(ProtocolCodec::encode(TaskAssign{"job", "c", 1, 20, 0, "f_####", "PNG"}).error,
              ProtocolError::InvalidPayload);
    EXPECT_EQ(ProtocolCodec::encode(
                  Progress{"job", "c", 1, 0, 1, std::numeric_limits<double>::quiet_NaN(), {}})
                  .error,
              ProtocolError::InvalidPayload);
    EXPECT_EQ(ProtocolCodec::encode(
                  FileBegin{"job", "PROJECT_ZIP", 0, std::string(64, 'a'), maxFileBlockBytes + 1})
                  .error,
              ProtocolError::InvalidPayload);
    EXPECT_EQ(ProtocolCodec::encode(ClientKick{std::string(1, static_cast<char>(0xff))}).error,
              ProtocolError::InvalidPayload);
}

TEST(ProtocolCodecTest, NullableFieldsLargeFilesAndDifferentHandshakeVersionRemainRepresentable) {
    const std::vector<Message> messages{
        Hello{42, "Alice", "PC", "token"},
        HelloOk{42, "source"},
        GpuInfo{"CPU", "other", 0, 1024, "macos", std::nullopt, "CPU", std::nullopt},
        ClientPrefs{{}, "DND", std::nullopt, 1, std::nullopt},
        Progress{"job", "c", -10, 0, 1, 0.0, std::nullopt},
        FileBegin{"job", "RESULT_ZIP", UINT64_MAX, std::string(64, 'a'), 1},
        FileAck{"job", UINT32_MAX}};
    for (const auto& message : messages) {
        const auto wire = ProtocolCodec::encode(message);
        ASSERT_TRUE(wire) << toString(wire.error);
        const auto result = ProtocolCodec::decode(*wire.value);
        ASSERT_TRUE(result) << toString(result.error);
        EXPECT_TRUE(result.value->message == message);
    }
}

TEST(ProtocolCodecTest, BinaryLengthMismatchesAndInvalidIdsAreRejected) {
    const auto encoded = ProtocolCodec::encode(FileData{"id", 1, {1, 2}});
    ASSERT_TRUE(encoded);
    for (const auto& [offset, value] : {std::pair<std::size_t, std::uint32_t>{12, 0},
                                        {12, UINT32_MAX},
                                        {22, 0},
                                        {22, 1},
                                        {22, 3},
                                        {22, UINT32_MAX}}) {
        auto wire = *encoded.value;
        put32(wire, offset, value);
        EXPECT_EQ(ProtocolCodec::decode(wire).error, ProtocolError::InvalidFileData);
    }
    auto invalidId = *encoded.value;
    invalidId[16] = 0xff;
    EXPECT_EQ(ProtocolCodec::decode(invalidId).error, ProtocolError::InvalidFileData);
    invalidId[16] = 0;
    EXPECT_EQ(ProtocolCodec::decode(invalidId).error, ProtocolError::InvalidFileData);
    EXPECT_EQ(ProtocolCodec::decode(frame(MessageType::FileData, "short")).error,
              ProtocolError::InvalidFileData);
    EXPECT_EQ(ProtocolCodec::encode(FileData{"", 0, {1}}).error, ProtocolError::InvalidFileData);
    EXPECT_EQ(ProtocolCodec::encode(FileData{"id", 0, {}}).error, ProtocolError::InvalidFileData);
}

TEST(ProtocolCodecTest, DeterministicMalformedInputCorpusNeverProducesUncheckedFrames) {
    std::mt19937 generator(42);
    for (int iteration = 0; iteration < 1000; ++iteration) {
        Bytes bytes(generator() % 256);
        for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(generator());
        }
        // En-tête valide une fois sur deux pour exercer aussi le parseur JSON/binaire.
        if (iteration % 2 == 0 && bytes.size() >= 12) {
            bytes[0] = 0x52;
            bytes[1] = 0x53;
            bytes[2] = 0;
            bytes[3] = static_cast<std::uint8_t>(iteration % 4 == 0 ? MessageType::Hello
                                                                    : MessageType::FileData);
            put32(bytes, 4, 0);
            put32(bytes, 8, static_cast<std::uint32_t>(bytes.size() - 12));
        }
        const auto result = ProtocolCodec::decode(bytes);
        if (result) {
            EXPECT_LE(result.value->bytesConsumed, bytes.size());
            EXPECT_TRUE(ProtocolCodec::encode(result.value->message));
        } else {
            EXPECT_NE(result.error, ProtocolError::None);
        }
    }
}
