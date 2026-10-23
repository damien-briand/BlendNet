#include "jobs/Job.h"

#include <iomanip>
#include <sstream>

namespace blendnet::jobs {
std::uint64_t FrameRange::count() const {
    if (step < 1 || end < start)
        return 0;
    return (std::int64_t(end) - start) / step + 1;
}
std::string_view toString(Job::State state) {
    switch (state) {
    case Job::State::Draft:
        return "Draft";
    case Job::State::Published:
        return "Published";
    case Job::State::Recruiting:
        return "Recruiting";
    case Job::State::Rendering:
        return "Rendering";
    case Job::State::Paused:
        return "Paused";
    case Job::State::Assembling:
        return "Assembling";
    case Job::State::Done:
        return "Done";
    case Job::State::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}
std::string frameName(int frame, std::string_view format) {
    std::ostringstream name;
    name << "frame_";
    if (frame < 0)
        name << '-';
    name << std::setfill('0') << std::setw(4) << (frame < 0 ? -std::int64_t(frame) : frame);
    name << (format == "PNG" ? ".png" : format == "JPEG" ? ".jpg" : ".exr");
    return name.str();
}
std::string resultFileId(std::string_view assignmentId) {
    return std::string(assignmentId) + "_RESULT_ZIP";
}
protocol::TaskAssign taskFor(const Job& job, const Chunk& chunk) {
    return {job.id,          chunk.assignmentId, chunk.frameStart, chunk.frameEnd,
            chunk.frameStep, "frame_####",       job.outputFormat};
}
} // namespace blendnet::jobs
