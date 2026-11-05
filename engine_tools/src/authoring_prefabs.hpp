#pragma once

#include "shark/tools/project_files.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string_view>

namespace shark::tools {

struct AuthoringPrefabFailure {
    int code;
    Error error;
};

struct AuthoringPrefabAdmission;

// Observation/preparation tokens are admitted only after the complete bounded
// protocol reply has been encoded. Published file writes are already retained.
struct AuthoringPrefabReply {
    nlohmann::json result;
    bool failed{false};
    bool scene_changed{false};

    explicit AuthoringPrefabReply(nlohmann::json value);
    ~AuthoringPrefabReply();
    AuthoringPrefabReply(AuthoringPrefabReply&&) noexcept;
    AuthoringPrefabReply& operator=(AuthoringPrefabReply&&) noexcept;
    AuthoringPrefabReply(const AuthoringPrefabReply&) = delete;
    AuthoringPrefabReply& operator=(const AuthoringPrefabReply&) = delete;

  private:
    std::unique_ptr<AuthoringPrefabAdmission> admission_;
    friend class AuthoringPrefabs;
};

// Session-private adapter borrowing the existing storage and authored owner.
// No independent Scene, catalog, importer or mutation retry ledger is created.
class AuthoringPrefabs final {
  public:
    AuthoringPrefabs(ProjectFiles& files, const ProjectDocument& project,
                     const ProjectPath& project_path, AuthoredScene& owner);
    ~AuthoringPrefabs();
    AuthoringPrefabs(const AuthoringPrefabs&) = delete;
    AuthoringPrefabs& operator=(const AuthoringPrefabs&) = delete;

    [[nodiscard]] static bool supports(std::string_view method) noexcept;
    [[nodiscard]] static nlohmann::json methods();
    [[nodiscard]] AuthoringPrefabReply dispatch(std::string_view method,
                                                const nlohmann::json& parameters,
                                                bool& outcome_may_have_changed);
    void admit(AuthoringPrefabReply& reply);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
