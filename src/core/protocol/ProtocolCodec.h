#pragma once

#include "protocol/Messages.h"

#include <span>

namespace blendnet::protocol {

enum class ProtocolError {
    None,
    NeedMoreData,
    InvalidMagic,
    InvalidReserved,
    UnknownMessageType,
    InvalidFlags,
    PayloadTooLarge,
    InvalidJson,
    InvalidPayload,
    InvalidFileData,
    TrailingData
};

// Messages fixes : aucune erreur ne contient le payload ni un token (RNF-S2).
std::string_view toString(ProtocolError error);

template <typename T> struct CodecResult {
    std::optional<T> value;
    ProtocolError error = ProtocolError::None;
    explicit operator bool() const { return value.has_value(); }
};

struct FrameHeader {
    MessageType type;
    std::uint32_t payloadBytes;
};

struct DecodedMessage {
    Message message;
    std::size_t bytesConsumed;
};

// P2 / §5.2 : point unique de lecture/écriture du framing et des schémas.
// Le codec n'ouvre aucun socket. En P3, le transport TLS journalisera l'erreur typée et
// fermera la connexion sur toute erreur fatale. NeedMoreData attend davantage d'octets.
class ProtocolCodec {
  public:
    static MessageType typeOf(const Message& message);
    static CodecResult<Bytes> encode(const Message& message);
    // Vérification de l'en-tête AVANT l'allocation du payload par le futur transport.
    static CodecResult<FrameHeader> decodeHeader(std::span<const std::uint8_t> bytes);
    static CodecResult<Message> decodePayload(const FrameHeader& header,
                                              std::span<const std::uint8_t> bytes);
    // Décode la première trame ; bytesConsumed permet de traiter un flux concaténé.
    static CodecResult<DecodedMessage> decode(std::span<const std::uint8_t> bytes);
};

} // namespace blendnet::protocol
