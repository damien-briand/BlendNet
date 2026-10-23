#pragma once

#include "protocol/Messages.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace blendnet::jobs {
struct JobTimings {
    std::chrono::milliseconds offerTimeout{60000};
    std::chrono::milliseconds taskTimeout{300000};
};
struct FrameRange {
    int start = 1, end = 1, step = 1;
    std::uint64_t count() const;
};
struct Job {
    std::string id;
    std::string projectName;
    std::string blendPath;
    std::vector<std::string> dependencies;
    std::string engine = "CYCLES";
    int frameStart = 1, frameEnd = 1, frameStep = 1;
    int chunkSize = 20;
    std::string outputFormat = "PNG";
    std::string blenderVersion = "4.2.1";
    enum class State {
        Draft,
        Published,
        Recruiting,
        Rendering,
        Paused,
        Assembling,
        Done,
        Cancelled
    };
    State state = State::Draft;
    std::filesystem::path projectZip;
    std::filesystem::path outputDirectory;
    std::uint64_t zipBytes = 0;
    int minimumVramMB = 0;
};
struct ValidatedFrame {
    int frame = 0;
    std::filesystem::path path;
    std::uint32_t width = 0, height = 0;
};
struct Chunk {
    std::string id;
    int frameStart = 1, frameEnd = 1, frameStep = 1;
    enum class State { Pending, Assigned, Downloading, Rendering, Uploading, Done, Failed };
    State state = State::Pending;
    std::string assignedClientId;
    int attempts = 0;         // Nombre d'échecs, jamais le nombre de simples offres.
    std::string assignmentId; // Change à chaque tentative : rejeter les résultats tardifs.
    int framesDone = 0;
};
struct JobSnapshot {
    Job job;
    std::vector<Chunk> chunks;
    std::uint64_t totalFrames = 0, validFrames = 0;
    std::vector<std::string> failures;
    bool finalizedWithFailures = false;
};
std::string_view toString(Job::State state);
std::string frameName(int frame, std::string_view format);
std::string resultFileId(std::string_view assignmentId);
protocol::TaskAssign taskFor(const Job& job, const Chunk& chunk);
} // namespace blendnet::jobs
