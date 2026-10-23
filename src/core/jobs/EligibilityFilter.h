#pragma once

#include "jobs/Job.h"
#include "network/NetworkTypes.h"

namespace blendnet::jobs {
class EligibilityFilter {
  public:
    // RF-20/21/25/51 : les limites concernent les frames réellement rendues du chunk.
    static bool isEligible(const network::ClientInfo& client, const Job& job,
                           const FrameRange& chunk);
};
} // namespace blendnet::jobs
