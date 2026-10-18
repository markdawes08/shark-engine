#pragma once

#include "report.hpp"

#include <nvrhi/nvrhi.h>

#include <filesystem>

namespace shark::probe {

struct ReadbackResults final {
    bool passed = false;
    std::vector<Json::Object> cases;
};

// Private assignment-02 fixture. Shaders are independently validated offline;
// the caller owns the validated NVRHI device and bounds the child process.
ReadbackResults runReadbackCases(nvrhi::IDevice* device,
                                 const std::filesystem::path& shaderDirectory,
                                 const std::filesystem::path& outputDirectory,
                                 bool injectReadbackMismatch = false);

} // namespace shark::probe
