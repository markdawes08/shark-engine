#include <shark/client/scene_content.hpp>

#include "shader_path.hpp"

#include <fstream>
#include <new>
#include <set>
#include <system_error>

namespace shark::client {
namespace {
constexpr std::string_view operation = "client.scene.load";

Error failure(ErrorCode code, std::string message, const std::string& path = {}) {
    return {code, std::move(message), {std::string{operation}, {}, path}};
}

Result<std::string> path_text(const std::filesystem::path& path) {
    auto text = detail::artifact_path_text(path);
    if (!text) {
        return failure(ErrorCode::invalid_path,
                       "Prepared content path must be bounded valid native Unicode without NUL");
    }
    return text;
}

Result<std::vector<std::byte>> read_payload(const std::filesystem::path& path,
                                            const std::string& text, std::size_t limit,
                                            std::size_t& remaining) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status)) {
        return failure(ErrorCode::io_failure, "Prepared content must be an accessible regular file",
                       text);
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return failure(ErrorCode::io_failure, "Cannot open prepared content", text);
    }
    const auto end = stream.tellg();
    if (end < 0) {
        return failure(ErrorCode::io_failure, "Cannot size prepared content", text);
    }
    if (end > static_cast<std::streamoff>(limit) || end > static_cast<std::streamoff>(remaining)) {
        return failure(ErrorCode::invalid_argument,
                       "Prepared content exceeds its file or aggregate input byte bound", text);
    }
    const auto size = static_cast<std::size_t>(end);
    remaining -= size;
    std::vector<std::byte> bytes(size);
    stream.seekg(0);
    if (!stream ||
        (size != 0 &&
         !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
        return failure(ErrorCode::io_failure,
                       "Cannot read a complete bounded prepared content snapshot", text);
    }
    return bytes;
}

Error at_path(Error error, const std::string& path) {
    error.context.path = path;
    return error;
}

Result<void> charge_file(const std::filesystem::path& path, const std::string& text,
                         std::size_t limit, std::size_t& remaining) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status)) {
        return failure(ErrorCode::io_failure, "Prepared content must be an accessible regular file",
                       text);
    }
    const auto size = std::filesystem::file_size(path, status);
    if (status) {
        return failure(ErrorCode::io_failure, "Cannot size prepared content", text);
    }
    if (size > limit || size > remaining) {
        return failure(ErrorCode::invalid_argument,
                       "Prepared content exceeds its file or aggregate input byte bound", text);
    }
    remaining -= static_cast<std::size_t>(size);
    return {};
}
} // namespace

Result<std::unique_ptr<LoadedPreparedScene>> load_scene_content(const SceneContentFiles& files) {
    std::string current_path;
    try {
        if (files.meshes.size() > prepared_assets_maximum_count - 2) {
            return failure(ErrorCode::invalid_argument,
                           "Prepared content admits at most 126 external mesh files");
        }
        auto scene_text = path_text(files.scene);
        if (!scene_text) {
            return scene_text.error();
        }
        // Validate the complete path inventory before opening any file. This
        // inventory is bounded independently of file lengths and decoding.
        std::vector<std::string> mesh_text;
        mesh_text.reserve(files.meshes.size());
        for (const auto& path : files.meshes) {
            auto text = path_text(path);
            if (!text) {
                return text.error();
            }
            mesh_text.push_back(std::move(text).value());
        }
        current_path = scene_text.value();
        std::size_t remaining = prepared_assets_maximum_bytes;
        if (auto charged = charge_file(files.scene, current_path,
                                       prepared_scene_maximum_payload_bytes, remaining);
            !charged) {
            return charged.error();
        }
        for (std::size_t index = 0; index < files.meshes.size(); ++index) {
            current_path = mesh_text[index];
            if (auto charged = charge_file(files.meshes[index], current_path,
                                           prepared_mesh_maximum_payload_bytes, remaining);
                !charged) {
                return charged.error();
            }
        }
        // Precharge the whole inventory before decoding. Actual reads size the
        // opened files again; replacement/growth cannot bypass the same bounds.
        remaining = prepared_assets_maximum_bytes;
        current_path = scene_text.value();
        auto scene_bytes = read_payload(files.scene, current_path,
                                        prepared_scene_maximum_payload_bytes, remaining);
        if (!scene_bytes) {
            return scene_bytes.error();
        }
        std::vector<std::vector<std::byte>> payloads;
        std::vector<PreparedAssetInput> inputs;
        payloads.reserve(files.meshes.size());
        inputs.reserve(files.meshes.size());
        std::set<AssetId> identities{builtin_cube_asset_id(), builtin_plane_asset_id()};
        for (std::size_t index = 0; index < files.meshes.size(); ++index) {
            current_path = mesh_text[index];
            auto bytes = read_payload(files.meshes[index], current_path,
                                      prepared_mesh_maximum_payload_bytes, remaining);
            if (!bytes) {
                return bytes.error();
            }
            // Decode through the Core boundary to obtain the embedded identity;
            // never treat unchecked envelope offsets as an asset declaration.
            auto decoded = decode_prepared_mesh(bytes.value());
            if (!decoded) {
                return at_path(decoded.error(), current_path);
            }
            const auto identity = decoded.value().asset;
            if (!identities.insert(identity).second) {
                auto error =
                    failure(ErrorCode::invalid_argument,
                            "Duplicate or engine-reserved prepared asset identity", current_path);
                error.context.asset_id = identity.value().to_string();
                return error;
            }
            payloads.push_back(std::move(bytes).value());
            inputs.push_back({identity, payloads.back()});
        }
        current_path = scene_text.value();
        auto assets = PreparedAssets::create(inputs);
        if (!assets) {
            return at_path(assets.error(), current_path);
        }
        auto loaded = LoadedPreparedScene::load(scene_bytes.value(), std::move(assets).value());
        if (!loaded) {
            return at_path(loaded.error(), current_path);
        }
        return loaded;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::execution_failed,
                       "Cannot allocate bounded prepared content candidate", current_path);
    } catch (const std::system_error&) {
        return failure(ErrorCode::io_failure, "Prepared content file access failed", current_path);
    } catch (...) {
        return failure(ErrorCode::execution_failed, "Prepared content candidate failed",
                       current_path);
    }
}

} // namespace shark::client
