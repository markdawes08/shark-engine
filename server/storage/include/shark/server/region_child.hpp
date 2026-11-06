#pragma once

#include "shark/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shark::server {
inline constexpr std::size_t region_child_maximum_frame_bytes = 16 * 1024 * 1024;
// One actual owned Server child. Launch always supplies --region-worker before
// bounded trusted startup arguments. The private inherited stdin/stdout stream
// uses uint32 big-endian byte lengths. Stderr is separate from protocol data.
class RegionChild final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RegionChild>>
    launch(const std::filesystem::path& executable, std::span<const std::string> arguments = {},
           bool inherit_diagnostics = false);
    ~RegionChild();
    RegionChild(const RegionChild&) = delete;
    RegionChild& operator=(const RegionChild&) = delete;
    [[nodiscard]] Result<std::vector<std::byte>> request(std::span<const std::byte> bytes,
                                                         std::chrono::milliseconds timeout);
    [[nodiscard]] Result<bool> alive() const;
    // Empty while running; the observed native exit status is retained after
    // reaping. POSIX signals map to 128+signal; Windows keeps signed status bits.
    [[nodiscard]] Result<std::optional<int>> exit_status() const;
    // Success confirms actual child exit. A timeout/failed RPC is not an exit
    // receipt and poisons the stream until explicit termination/replacement.
    [[nodiscard]] Result<void> terminate(std::chrono::milliseconds timeout = std::chrono::seconds{
                                             5});
    [[nodiscard]] std::uint64_t process_id() const noexcept;

  private:
    struct Impl;
    explicit RegionChild(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace shark::server
