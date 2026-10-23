#pragma once

#include "blender/BlenderTypes.h"

namespace blendnet::blender {
struct ComputeDevice {
    std::string name, backend, vendor;
    int vramMB = 0;
};
struct HardwareProfile {
    protocol::GpuInfo gpu;
    std::vector<ComputeDevice> devices;
    std::vector<std::string> availableBackends;
    std::string eeveeEngine;
    int frameMinimum = 0, frameMaximum = 1048574, stepMaximum = 1048574;
};
class GpuInfoService {
  public:
    static BlenderResult<HardwareProfile> probe(const VerifiedBlender& binary,
                                                std::string_view selectedBackend = "CPU",
                                                const std::atomic_bool* cancelled = nullptr);
    static bool validBackend(std::string_view backend);
};
} // namespace blendnet::blender
