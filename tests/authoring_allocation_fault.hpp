#pragma once
#include <cstddef>
namespace shark::test::allocation_fault {
void arm(std::size_t index) noexcept;
[[nodiscard]] bool disarm() noexcept;
} // namespace shark::test::allocation_fault
