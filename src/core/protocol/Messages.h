#pragma once

#include "protocol/MessageTypes.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace blendnet::protocol {

using Bytes = std::vector<std::uint8_t>;

// DTO du protocole uniquement ; les états métier appartiennent aux phases P3 à P7.
struct Hello {
    static constexpr auto type = MessageType::Hello;
    std::uint32_t protocolVersion = protocol::protocolVersion;
    std::string userName;
    std::string machineName;
    std::string token;
    bool operator==(const Hello&) const = default;
};

struct HelloOk {
    static constexpr auto type = MessageType::HelloOk;
    std::uint32_t protocolVersion = protocol::protocolVersion;
    std::string serverName;
    bool operator==(const HelloOk&) const = default;
};

struct HelloErr {
    static constexpr auto type = MessageType::HelloErr;
    std::string reason;
    bool operator==(const HelloErr&) const = default;
};

struct GpuInfo {
    static constexpr auto type = MessageType::GpuInfo;
    std::string gpuModel;
    std::string gpuVendor;
    int vramMB = 0;
    int ramMB = 0;
    std::string os;
    std::optional<std::string> blenderVersion;
    std::string backend;
    std::optional<int> benchmarkScore;
    bool operator==(const GpuInfo&) const = default;
};

struct ClientPrefs {
    static constexpr auto type = MessageType::ClientPrefs;
    std::vector<std::string> excludedEngines;
    std::string availability;
    std::optional<int> maxFramesPerJob;
    int maxConcurrentJobs = 1;
    std::optional<int> quotaGB;
    bool operator==(const ClientPrefs&) const = default;
};

struct Ping {
    static constexpr auto type = MessageType::Ping;
    bool operator==(const Ping&) const = default;
};

struct Pong {
    static constexpr auto type = MessageType::Pong;
    bool operator==(const Pong&) const = default;
};

struct JobOffer {
    static constexpr auto type = MessageType::JobOffer;
    std::string jobId;
    std::string projectName;
    std::string engine;
    int frameStart = 1;
    int frameEnd = 1;
    int frameStep = 1;
    std::uint64_t zipBytes = 0;
    std::string blenderVersion;
    std::string outputFormat;
    bool operator==(const JobOffer&) const = default;
};

struct JobAccept {
    static constexpr auto type = MessageType::JobAccept;
    std::string jobId;
    bool operator==(const JobAccept&) const = default;
};

struct JobDecline {
    static constexpr auto type = MessageType::JobDecline;
    std::string jobId;
    bool operator==(const JobDecline&) const = default;
};

struct TaskAssign {
    static constexpr auto type = MessageType::TaskAssign;
    std::string jobId;
    std::string chunkId;
    int frameStart = 1;
    int frameEnd = 1;
    int frameStep = 1;
    std::string outputPattern;
    std::string outputFormat;
    bool operator==(const TaskAssign&) const = default;
};

struct Progress {
    static constexpr auto type = MessageType::Progress;
    std::string jobId;
    std::string chunkId;
    int currentFrame = 1;
    int framesDone = 0;
    int framesTotal = 1;
    double percent = 0.0;
    std::optional<int> etaSec;
    bool operator==(const Progress&) const = default;
};

struct TaskCancel {
    static constexpr auto type = MessageType::TaskCancel;
    std::string jobId;
    std::string chunkId;
    bool operator==(const TaskCancel&) const = default;
};

struct JobPause {
    static constexpr auto type = MessageType::JobPause;
    std::string jobId;
    bool operator==(const JobPause&) const = default;
};

struct JobResume {
    static constexpr auto type = MessageType::JobResume;
    std::string jobId;
    bool operator==(const JobResume&) const = default;
};

struct ClientKick {
    static constexpr auto type = MessageType::ClientKick;
    std::string reason;
    bool operator==(const ClientKick&) const = default;
};

struct StatusUpdate {
    static constexpr auto type = MessageType::StatusUpdate;
    ClientPrefs preferences;
    bool operator==(const StatusUpdate&) const = default;
};

struct FileBegin {
    static constexpr auto type = MessageType::FileBegin;
    std::string fileId;
    std::string kind;
    std::uint64_t totalBytes = 0;
    std::string sha256;
    std::uint32_t chunkSize = maxFileBlockBytes;
    bool operator==(const FileBegin&) const = default;
};

struct FileData {
    static constexpr auto type = MessageType::FileData;
    std::string fileId;
    std::uint32_t seq = 0;
    Bytes data;
    bool operator==(const FileData&) const = default;
};

struct FileEnd {
    static constexpr auto type = MessageType::FileEnd;
    std::string fileId;
    std::string sha256;
    bool operator==(const FileEnd&) const = default;
};

struct FileAck {
    static constexpr auto type = MessageType::FileAck;
    std::string fileId;
    std::uint32_t nextSeq = 0;
    bool operator==(const FileAck&) const = default;
};

struct FileErr {
    static constexpr auto type = MessageType::FileErr;
    std::string fileId;
    std::string reason;
    bool operator==(const FileErr&) const = default;
};

using Message =
    std::variant<Hello, HelloOk, HelloErr, GpuInfo, ClientPrefs, Ping, Pong, JobOffer, JobAccept,
                 JobDecline, TaskAssign, Progress, TaskCancel, JobPause, JobResume, ClientKick,
                 StatusUpdate, FileBegin, FileData, FileEnd, FileAck, FileErr>;

} // namespace blendnet::protocol
