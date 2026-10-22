#pragma once

#include "Cli.h"
#include "config/Utf8Path.h"
#include "protocol/ProtocolCodec.h"
#include "transfer/TransferTypes.h"

#include <iostream>

namespace blendnet::cli {
struct TransferArguments {
    transfer::FileRequest file;
    transfer::TransferOptions storage;
    bool exitAfter = false;
    bool consume(std::string_view argument, std::string_view value) {
        if (argument == "--transfer-dir")
            storage.directory = pathFromUtf8(value);
        else if (argument == "--send-file")
            file.path = pathFromUtf8(value);
        else if (argument == "--file-id")
            file.fileId = value;
        else if (argument == "--kind")
            file.kind = value;
        else if (argument == "--quota-bytes") {
            std::uint64_t count = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return false;
            storage.quotaBytes = count;
        } else
            return false;
        return true;
    }
    bool valid() const {
        if (file.path.empty())
            return file.fileId.empty();
        return static_cast<bool>(protocol::ProtocolCodec::encode(
            protocol::FileBegin{file.fileId, file.kind, 0, std::string(64, '0'), file.chunkSize}));
    }
};
inline bool printTransfer(const transfer::TransferProgress& progress) {
    const char* direction = progress.direction == transfer::Direction::Sending ? "SEND" : "RECEIVE";
    std::cout << "TRANSFER " << direction << ' ' << progress.completedBytes << ' '
              << progress.totalBytes << ' ' << progress.resumedBytes << '\n'
              << std::flush;
    if (progress.state == transfer::TransferState::Completed)
        std::cout << "TRANSFER_DONE " << direction << '\n' << std::flush;
    if (progress.state == transfer::TransferState::Failed) {
        std::cerr << transfer::toString(progress.error) << '\n';
        return false;
    }
    return true;
}
inline constexpr std::string_view transferHelp =
    "  --send-file PATH --file-id ID [--kind PROJECT_ZIP|RESULT_ZIP]\n"
    "  --transfer-dir PATH [--quota-bytes N]  Set receive storage and quota.\n"
    "  --exit-after-transfer  Exit after a validated transfer completes.\n";
} // namespace blendnet::cli
