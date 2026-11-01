#pragma once

#include "authoring_assets.hpp"
#include "shark/tools/authoring_protocol.hpp"

#include <functional>

namespace shark::tools {

// Private retained preview owner. Runtime bodies never borrow the authored Scene.
class AuthoringPhysics final {
  public:
    AuthoringPhysics();
    ~AuthoringPhysics();
    AuthoringPhysics(const AuthoringPhysics&) = delete;
    AuthoringPhysics& operator=(const AuthoringPhysics&) = delete;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool replays_start(const nlohmann::json& parameters) const;
    [[nodiscard]] Result<nlohmann::json>
    dispatch(const nlohmann::json& parameters, const AuthoredScene& owner, AuthoringAssets& assets,
             bool& outcome_may_have_changed, const std::function<Result<void>()>& validate_source);
    [[nodiscard]] Result<RenderScene> snapshot(const AuthoringViewportRequest& request,
                                               const AuthoredScene& owner) const;
    [[nodiscard]] static nlohmann::json method_schema();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
