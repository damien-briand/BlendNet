#pragma once

#include "jobs/Job.h"

#include <atomic>

namespace blendnet::jobs {
class ResultAssembler {
  public:
    static std::optional<ValidatedFrame> inspect(int frame, const std::filesystem::path& path,
                                                 std::string_view format,
                                                 const std::atomic_bool* cancelled = nullptr);
    // RF-53/RNF-S4 : uniquement des noms attendus, limites avant extraction, CRC ZIP.
    static std::vector<ValidatedFrame> validate(const protocol::TaskAssign& task,
                                                const std::filesystem::path& archive,
                                                const std::filesystem::path& staging,
                                                const std::atomic_bool* cancelled = nullptr);
    static bool assemble(const Job& job, const std::map<int, ValidatedFrame>& frames,
                         const std::atomic_bool* cancelled = nullptr, bool allowMissing = false);
    // Fixture du RenderProcess simulé, jamais utilisée comme rendu Blender réel.
    static bool simulatedResult(const protocol::TaskAssign& task,
                                const std::filesystem::path& archive);
};
} // namespace blendnet::jobs
