#pragma once

#include "jobs/Job.h"

namespace blendnet::jobs {
// Configuration locale de démo P5 ; aucun chemin reçu du réseau.
std::optional<Job> loadJob(const std::filesystem::path& file);
} // namespace blendnet::jobs
