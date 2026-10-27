#include "protocol/ProtocolCodec.h"

#include <cstdlib>

namespace {
void check(const blendnet::protocol::Message& message) {
    using namespace blendnet::protocol;
    const auto encoded = ProtocolCodec::encode(message);
    if (!encoded)
        std::abort();
    const auto decoded = ProtocolCodec::decode(*encoded.value);
    if (!decoded || decoded.value->bytesConsumed != encoded.value->size())
        std::abort();
    const auto canonical = ProtocolCodec::encode(decoded.value->message);
    if (!canonical || *canonical.value != *encoded.value)
        std::abort();
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace blendnet::protocol;
    if (size > headerBytes + maxFileDataBytes)
        return 0;
    const std::span<const std::uint8_t> bytes(data, size);
    if (const auto message = ProtocolCodec::decode(bytes))
        check(message.value->message);
    // A second entry path reaches every message schema without requiring a
    // mutation engine to first discover the magic bytes and the length field.
    if (!bytes.empty()) {
        const FrameHeader header{static_cast<MessageType>(bytes[0]),
                                 static_cast<std::uint32_t>(size - 1)};
        if (const auto message = ProtocolCodec::decodePayload(header, bytes.subspan(1)))
            check(*message.value);
    }
    return 0;
}
