#pragma once

#include "shark/tools/authored_scene.hpp"

namespace shark::tools::detail {

// Private invariant-fixture access for pending work, visits and closed owners.
// Never expose this header or mutable borrow through public authoring adapters.
struct AuthoredSceneTestAccess {
    [[nodiscard]] static Result<std::reference_wrapper<Scene>> scene(AuthoredScene& owner);
    // One-shot allocation exception at the Nth batch preparation boundary.
    [[nodiscard]] static Result<void> fail_batch_after(AuthoredScene& owner, std::size_t step);
    [[nodiscard]] static Result<void> fail_history_after(AuthoredScene& owner, std::size_t step);
    [[nodiscard]] static Result<void> fail_mutation_after(AuthoredScene& owner, std::size_t step);
    [[nodiscard]] static Result<void> mutation_watermark(AuthoredScene& owner, std::uint64_t value);
};

} // namespace shark::tools::detail
