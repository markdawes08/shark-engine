#ifndef _WIN32
// Test-executable-only allocator replacement. Fail one allocation on the armed
// owner thread, then restore normal allocation before constructing diagnostics.
#include "authoring_allocation_fault.hpp"
#include <cstdlib>
#include <limits>
#include <new>

namespace {
thread_local std::size_t remaining = std::numeric_limits<std::size_t>::max();
thread_local bool fired = false;
} // namespace
namespace shark::test::allocation_fault {
void arm(std::size_t index) noexcept {
    remaining = index;
    fired = false;
}
bool disarm() noexcept {
    remaining = std::numeric_limits<std::size_t>::max();
    return fired;
}
} // namespace shark::test::allocation_fault
void* operator new(std::size_t bytes) {
    if (remaining != std::numeric_limits<std::size_t>::max()) {
        if (remaining == 0) {
            remaining = std::numeric_limits<std::size_t>::max();
            fired = true;
            throw std::bad_alloc{};
        }
        --remaining;
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

#endif
