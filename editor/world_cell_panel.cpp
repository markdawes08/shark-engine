#include "world_cell_panel.hpp"

#include <nlohmann/json.hpp>

#include <array>

namespace shark::editor {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
};
template <class T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
Json call(ProjectSession& owner, const char* method, const Json& parameters) {
    const auto reply = take(owner.request(method, parameters.dump()));
    const auto document = Json::parse(reply.json);
    if (document.contains("error")) {
        const auto& error = document.at("error");
        const auto& value = error.at("data");
        Error decoded{ErrorCode::execution_failed,
                      error.at("message").get<std::string>(),
                      {"editor.world-cell"}};
        constexpr std::array codes{ErrorCode::invalid_argument,
                                   ErrorCode::invalid_configuration,
                                   ErrorCode::invalid_id,
                                   ErrorCode::invalid_path,
                                   ErrorCode::path_outside_root,
                                   ErrorCode::io_failure,
                                   ErrorCode::entropy_unavailable,
                                   ErrorCode::capability_unavailable,
                                   ErrorCode::conflict,
                                   ErrorCode::execution_failed};
        for (const auto code : codes)
            if (value.at("code") == error_code_name(code))
                decoded.code = code;
        const auto& context = value.at("context");
        decoded.context = {
            context.value("operation", std::string{}), context.value("request_id", std::string{}),
            context.value("path", std::string{}), context.value("entity_id", std::string{}),
            context.value("asset_id", std::string{})};
        throw Failure{std::move(decoded)};
    }
    if (reply.failed)
        throw Failure{Error{ErrorCode::execution_failed,
                            "Cell operation rejected; inspect output before retrying",
                            {"editor.world-cell"}}};
    return document.at("result");
}
WorldCellArtifact artifact(const Json& value, bool published) {
    WorldCellArtifact result{value.at("path").get<std::string>(),
                             value.at("payload_digest").get<std::string>(),
                             value.at("payload_bytes").get<std::size_t>(),
                             value.at("vertices").get<std::size_t>(),
                             value.at("collision_triangles").get<std::size_t>(),
                             value.at("navigation_triangles").get<std::size_t>(),
                             published};
    if (published) {
        const auto& publication = value.at("publication");
        if (!publication.at("published").get<bool>())
            throw Failure{Error{ErrorCode::execution_failed,
                                "Cell publication was not confirmed",
                                {"editor.world-cell"}}};
        const auto kind = publication.at("durability").get<std::string>();
        if (kind == "file_flushed")
            result.durability = tools::FileDurability::file_flushed;
        else if (kind == "file_and_directory_flushed")
            result.durability = tools::FileDurability::file_and_directory_flushed;
        else if (kind == "directory_flush_failed")
            result.durability = tools::FileDurability::directory_flush_failed;
        else
            throw Failure{Error{ErrorCode::execution_failed,
                                "Cell publication has an unknown durability; inspect output",
                                {"editor.world-cell"}}};
    }
    return result;
}
template <class F> Result<WorldCellArtifact> boundary(F&& f) {
    try {
        return f();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot complete cell operation; inspect output before retrying",
                     {"editor.world-cell"}};
    }
}
} // namespace

Result<WorldCellArtifact> WorldCellPanel::export_cell(const WorldCellExportOptions& options,
                                                      SceneRevision expected) {
    return boundary([&] {
        if (options.collision.empty() || options.collision.size() > 256 ||
            options.navigation.size() > 256 || options.path.empty() || options.path.size() > 4096 ||
            (options.target.side != tools::AssetAudience::server &&
             options.target.side != tools::AssetAudience::client))
            throw Failure{Error{ErrorCode::invalid_argument,
                                "Cell options exceed the supported bounds",
                                {"editor.world-cell"}}};
        const auto catalog = call(session_, "asset.catalog.status", Json::object());
        const auto cache = call(session_, "asset.cache.status", Json::object());
        prepared_.reset();
        Json collision = Json::array(), navigation = Json::array();
        for (const auto id : options.collision)
            collision.push_back(id.value().to_string());
        for (const auto id : options.navigation)
            navigation.push_back(id.value().to_string());
        Json parameters{
            {"expected_revision",
             {{"scene_instance", std::to_string(expected.scene_instance)},
              {"revision", std::to_string(expected.revision)}}},
            {"expected_catalog", catalog.at("revision")},
            {"expected_cache", cache.at("revision")},
            {"world_id", options.key.world.value().to_string()},
            {"cell",
             {{"x", options.key.cell.x}, {"y", options.key.cell.y}, {"z", options.key.cell.z}}},
            {"content_revision", std::to_string(options.content_revision)},
            {"target",
             {{"id", options.target.id},
              {"side", options.target.side == tools::AssetAudience::server ? "server" : "client"}}},
            {"collision_entities", std::move(collision)},
            {"navigation_entities", std::move(navigation)}};
        auto prepared = call(session_, "scene.cell.prepare", parameters);
        prepared["path"] = options.path;
        prepared_ = artifact(prepared, false);
        parameters["path"] = options.path;
        parameters["expected_digest"] = prepared_->digest;
        const auto result = call(session_, "scene.cell.export", parameters);
        return artifact(result, true);
    });
}
Result<WorldCellArtifact> WorldCellPanel::inspect(std::string_view path) {
    return boundary([&] {
        auto result = artifact(call(session_, "cell.inspect", {{"path", path}}), false);
        if (prepared_ && prepared_->path == result.path)
            result.matches_prepared = prepared_->digest == result.digest;
        return result;
    });
}
} // namespace shark::editor
