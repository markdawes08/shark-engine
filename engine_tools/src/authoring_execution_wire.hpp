#pragma once

#include "authoring_export.hpp"
#include "authoring_run.hpp"
#include <nlohmann/json.hpp>

namespace shark::tools::authoring_execution_wire {
[[nodiscard]] Result<AuthoringRunRequest> parse_run(const nlohmann::json& value);
[[nodiscard]] Result<ClientExportRequest> parse_content(const nlohmann::json& value);
[[nodiscard]] Result<AuthoringExportRequest> parse_export(const nlohmann::json& value);
[[nodiscard]] Result<AuthoringCaptureRequest> parse_capture(const nlohmann::json& value);
[[nodiscard]] nlohmann::json methods(bool export_available, bool capture_available);
} // namespace shark::tools::authoring_execution_wire
