#pragma once

#include "protocol/Messages.h"

#include <filesystem>
#include <optional>
#include <string>

namespace blendnet::transfer {
inline constexpr std::uint32_t ackBatchBlocks = 4;
inline constexpr std::uint32_t windowBlocks = 8;

enum class TransferError {
    None,
    InvalidRequest,
    IoError,
    DiskFull,
    ChecksumMismatch,
    InvalidSequence,
    ConflictingOffer,
    CorruptState,
    Busy,
    Aborted
};
std::string_view toString(TransferError error);
enum class TransferState { Preparing, Transferring, Suspended, Completed, Failed };
enum class Direction { Sending, Receiving };
struct TransferOptions {
    // Vide : transferts désactivés. Chemin privé réservé exclusivement aux fichiers reçus.
    std::filesystem::path directory;
    std::optional<std::uint64_t> quotaBytes;
};
struct FileRequest {
    std::filesystem::path path{};
    std::string fileId;
    std::string kind = "PROJECT_ZIP";
    std::uint32_t chunkSize = protocol::maxFileBlockBytes;
    bool operator==(const FileRequest&) const = default;
};
struct TransferProgress {
    std::string fileId;
    Direction direction = Direction::Receiving;
    TransferState state = TransferState::Preparing;
    TransferError error = TransferError::None;
    std::uint64_t totalBytes = 0;
    std::uint64_t completedBytes = 0;
    std::uint64_t resumedBytes = 0;
    std::filesystem::path path{};
    std::string peerId{};
};
template <typename T> struct TransferResult {
    std::optional<T> value;
    TransferError error = TransferError::None;
    explicit operator bool() const { return value.has_value(); }
};
} // namespace blendnet::transfer
