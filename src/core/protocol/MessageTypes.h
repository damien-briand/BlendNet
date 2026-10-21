#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace blendnet::protocol {

inline constexpr std::uint32_t protocolVersion = 1;
inline constexpr std::uint16_t protocolMagic = 0x5253;
// §5.2 / P2 : 2 + 1 + 1 + 4 + 4 = 12 octets, sans padding C++.
inline constexpr std::size_t headerBytes = 12;
inline constexpr std::uint32_t maxControlBytes = 64 * 1024;
inline constexpr std::uint32_t maxFileBlockBytes = 1024 * 1024;
inline constexpr std::uint32_t maxFileIdBytes = 256;
inline constexpr std::uint32_t fileDataPrefixBytes = 12;
inline constexpr std::uint32_t maxFileDataBytes =
    fileDataPrefixBytes + maxFileIdBytes + maxFileBlockBytes;

enum class MessageType : std::uint8_t {
    Hello = 0,
    HelloOk = 1,
    HelloErr = 2,
    GpuInfo = 3,
    ClientPrefs = 4,
    Ping = 5,
    Pong = 6,
    JobOffer = 10,
    JobAccept = 11,
    JobDecline = 12,
    TaskAssign = 20,
    Progress = 21,
    TaskCancel = 22,
    JobPause = 23,
    JobResume = 24,
    ClientKick = 25,
    StatusUpdate = 26,
    FileBegin = 30,
    FileData = 31,
    FileEnd = 32,
    FileAck = 33,
    FileErr = 34
};

std::string_view toString(MessageType type);

} // namespace blendnet::protocol
