#pragma once

#include "shark/tools/asset_import.hpp"

#include <nlohmann/json.hpp>

namespace shark::tools {

inline constexpr std::size_t authoring_asset_maximum_metadata_revisions = 32;

// Private adapter failures preserve native diagnostics and distinguish malformed
// protocol parameters from a rejected owner operation.
struct AuthoringAssetFailure {
    int code;
    Error error;
};

struct AuthoringAssetObservation {
    std::string token;
    ProjectPath path;
    FileRevision revision;
};

struct AuthoringAssetReply {
    nlohmann::json result;
    std::optional<AuthoringAssetObservation> observation{};
    std::optional<std::string> released_token{};
    std::optional<AssetImportJobId> taken_job{};
};

// Lives inside the retained protocol Session. Its import worker is joined before
// the borrowed project/catalog/importer owners are released. Cache opening is
// lazy so a corrupt disposable cache cannot block typed metadata repair.
class AuthoringAssets final {
  public:
    AuthoringAssets(ProjectFiles& files, const ProjectDocument& project, const ProjectPath& path);
    ~AuthoringAssets();
    AuthoringAssets(const AuthoringAssets&) = delete;
    AuthoringAssets& operator=(const AuthoringAssets&) = delete;

    [[nodiscard]] static bool supports(std::string_view method) noexcept;
    [[nodiscard]] static bool requires_project_fence(std::string_view method) noexcept;
    [[nodiscard]] AuthoringAssetReply dispatch(std::string_view method,
                                               const nlohmann::json& parameters,
                                               bool& outcome_may_have_changed);
    // A read token or take acknowledgement is admitted only after the enclosing
    // protocol has successfully encoded and bounded its entire response.
    void admit(AuthoringAssetReply& reply);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
