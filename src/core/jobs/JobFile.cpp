#include "jobs/JobFile.h"
#include "config/Utf8Path.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace blendnet::jobs {
std::optional<Job> loadJob(const std::filesystem::path& file) {
    try {
        if (std::filesystem::file_size(file) > 65536)
            return {};
        std::ifstream input(file);
        const auto data = nlohmann::json::parse(input);
        const std::set<std::string> allowed{
            "id",        "projectName",  "projectZip",     "outputDirectory",
            "engine",    "frameStart",   "frameEnd",       "frameStep",
            "chunkSize", "outputFormat", "blenderVersion", "minimumVramMB"};
        for (const auto& [key, value] : data.items())
            if (!allowed.contains(key))
                return {};
        Job job;
        job.id = data.at("id").get<std::string>();
        job.projectName = data.at("projectName").get<std::string>();
        job.projectZip = pathFromUtf8(data.at("projectZip").get<std::string>());
        job.outputDirectory = pathFromUtf8(data.at("outputDirectory").get<std::string>());
        if (job.projectZip.empty() || job.outputDirectory.empty())
            return {};
        if (job.projectZip.is_relative())
            job.projectZip = file.parent_path() / job.projectZip;
        if (job.outputDirectory.is_relative())
            job.outputDirectory = file.parent_path() / job.outputDirectory;
        job.engine = data.value("engine", job.engine);
        job.outputFormat = data.value("outputFormat", job.outputFormat);
        job.blenderVersion = data.value("blenderVersion", job.blenderVersion);
        const auto number = [&](const char* key, int fallback) {
            if (!data.contains(key))
                return fallback;
            const auto& n = data.at(key);
            if (!n.is_number_integer() || n < INT32_MIN || n > INT32_MAX)
                throw std::runtime_error("Invalid integer");
            return n.get<int>();
        };
        job.frameStart = number("frameStart", 1);
        job.frameEnd = number("frameEnd", 1);
        job.frameStep = number("frameStep", 1);
        job.chunkSize = number("chunkSize", 20);
        job.minimumVramMB = number("minimumVramMB", 0);
        return job;
    } catch (const std::exception&) {
        return {};
    }
}
} // namespace blendnet::jobs
