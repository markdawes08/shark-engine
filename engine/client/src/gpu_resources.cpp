#include <shark/client/gpu_resources.hpp>

#include "shader_path.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace shark::client {
namespace {

constexpr std::size_t max_shader_bytes = 4U * 1024U * 1024U;
constexpr std::uint32_t max_shader_ids = 65536;

Error resource_error(std::string message, std::string operation = "shader.artifact") {
    return {ErrorCode::invalid_argument, std::move(message), {std::move(operation)}};
}

struct Decoration {
    std::optional<std::uint32_t> location;
    std::optional<std::uint32_t> binding;
    std::optional<std::uint32_t> descriptor_set;
    std::optional<std::uint32_t> builtin;
    std::optional<std::uint32_t> offset;
    std::optional<std::uint32_t> stride;
    bool block{false};
    bool row_major{false};
    bool unsupported{false};
};

struct Node {
    std::uint32_t opcode{0};
    std::vector<std::uint32_t> operands;
    Decoration decoration;
    std::array<Decoration, 2> members;
};

bool assign(std::optional<std::uint32_t>& target, std::span<const std::uint32_t> words) {
    if (target || words.size() != 1) {
        return false;
    }
    target = words[0];
    return true;
}

bool decorate(Decoration& target, std::uint32_t kind, std::span<const std::uint32_t> values,
              bool member) {
    switch (kind) {
    case 2: // Block
        if (member || target.block || !values.empty()) {
            return false;
        }
        target.block = true;
        return true;
    case 4: // DXC lowers HLSL column_major to a transposed RowMajor SPIR-V matrix.
        if (!member || target.row_major || !values.empty()) {
            return false;
        }
        target.row_major = true;
        return true;
    case 7:
        return member && assign(target.stride, values);
    case 11:
        return !member && assign(target.builtin, values);
    case 30:
        return !member && assign(target.location, values);
    case 33:
        return !member && assign(target.binding, values);
    case 34:
        return !member && assign(target.descriptor_set, values);
    case 35:
        return member && assign(target.offset, values);
    default:
        target.unsupported = true;
        return true;
    }
}

class InterfaceReader {
  public:
    InterfaceReader(const ShaderArtifact& artifact, ShaderInterface shader_interface)
        : artifact_(artifact), interface_(shader_interface) {}

    Result<void> validate() {
        const auto& words = artifact_.words;
        if (words.size() < 5 || words.size() > max_shader_bytes / 4 || words[0] != 0x07230203 ||
            words[1] != 0x00010600 || words[4] != 0 || words[3] == 0 || words[3] > max_shader_ids) {
            return resource_error("Expected bounded little-endian SPIR-V 1.6 with a valid header");
        }
        if (artifact_.stage != ShaderStage::vertex && artifact_.stage != ShaderStage::pixel) {
            return resource_error("Unsupported shader stage", "shader.layout");
        }
        if (interface_ != ShaderInterface::textured_cube &&
            interface_ != ShaderInterface::scene_unlit && interface_ != ShaderInterface::gui) {
            return resource_error("Unsupported shader interface", "shader.layout");
        }
        const auto expected = interface_ == ShaderInterface::textured_cube
                                  ? (artifact_.stage == ShaderStage::vertex ? "vsMain" : "psMain")
                              : interface_ == ShaderInterface::scene_unlit
                                  ? (artifact_.stage == ShaderStage::vertex ? "vsScene" : "psScene")
                                  : (artifact_.stage == ShaderStage::vertex ? "vsGui" : "psGui");
        if (artifact_.entry_point != expected) {
            return resource_error("Shader entry point does not match the selected interface",
                                  "shader.layout");
        }
        nodes_.resize(words[3]);
        for (std::size_t offset = 5; offset < words.size();) {
            const auto count = static_cast<std::size_t>(words[offset] >> 16U);
            const auto opcode = words[offset] & 0xFFFFU;
            if (count == 0 || count > words.size() - offset) {
                return resource_error("Truncated or zero-length SPIR-V instruction");
            }
            const auto args = std::span(words).subspan(offset + 1, count - 1);
            if (!instruction(opcode, args)) {
                return resource_error("Malformed or unsupported SPIR-V interface instruction",
                                      "shader.layout");
            }
            offset += count;
        }
        if (!entry_seen_ || !memory_model_seen_ || !shader_capability_seen_ ||
            !entry_function_seen_ || in_function_) {
            return resource_error(
                "SPIR-V entry, complete function, memory model or capability is missing",
                "shader.layout");
        }
        return validate_variables();
    }

  private:
    bool valid_id(std::uint32_t id) const {
        return id != 0 && id < nodes_.size();
    }

    bool record(std::uint32_t opcode, std::uint32_t id, std::span<const std::uint32_t> operands) {
        if (!valid_id(id) || nodes_[id].opcode != 0 || operands.size() > 64) {
            return false;
        }
        nodes_[id].opcode = opcode;
        nodes_[id].operands.assign(operands.begin(), operands.end());
        return true;
    }

    bool entry(std::span<const std::uint32_t> args) {
        const auto model = artifact_.stage == ShaderStage::vertex ? 0U : 4U;
        if (entry_seen_ || args.size() < 3 || args[0] != model || !valid_id(args[1])) {
            return false;
        }
        entry_seen_ = true;
        entry_function_ = args[1];
        std::string name;
        std::size_t next = 2;
        bool terminated = false;
        for (; next < args.size() && !terminated; ++next) {
            const auto word = args[next];
            for (std::uint32_t shift = 0; shift < 32; shift += 8) {
                const auto value = (word >> shift) & 0xFFU;
                if (value == 0) {
                    // String padding is zero, not hidden interface bytes.
                    if (shift < 24 && (word >> (shift + 8)) != 0) {
                        return false;
                    }
                    terminated = true;
                    break;
                }
                name.push_back(static_cast<char>(value));
                if (name.size() > 64) {
                    return false;
                }
            }
        }
        if (!terminated || name != artifact_.entry_point) {
            return false;
        }
        for (; next < args.size(); ++next) {
            if (!valid_id(args[next]) || !entry_interfaces_.insert(args[next]).second) {
                return false;
            }
        }
        return true;
    }

    bool instruction(std::uint32_t opcode, std::span<const std::uint32_t> args) {
        if (opcode == 17) { // OpCapability: both fixed profiles require only Shader.
            if (args.size() != 1 || args[0] != 1 || shader_capability_seen_) {
                return false;
            }
            shader_capability_seen_ = true;
        } else if (opcode == 14) { // Logical, GLSL450.
            if (args.size() != 2 || args[0] != 0 || args[1] != 1 || memory_model_seen_) {
                return false;
            }
            memory_model_seen_ = true;
        } else if (opcode == 15) {
            return entry(args);
        } else if ((opcode >= 19 && opcode <= 33)) { // SPIR-V type declarations.
            return !args.empty() && record(opcode, args[0], args.subspan(1));
        } else if (opcode == 54) { // OpFunction.
            if (in_function_ || args.size() != 4 || !valid_id(args[0]) || !valid_id(args[1]) ||
                !valid_id(args[3])) {
                return false;
            }
            const std::array values{args[0], args[2], args[3]};
            if (!record(opcode, args[1], values)) {
                return false;
            }
            in_function_ = true;
            entry_function_seen_ = entry_function_seen_ || args[1] == entry_function_;
        } else if (opcode == 56) { // OpFunctionEnd.
            if (!in_function_ || !args.empty()) {
                return false;
            }
            in_function_ = false;
        } else if (opcode == 59) { // OpVariable.
            if (args.size() < 3 || args.size() > 4 || !valid_id(args[0])) {
                return false;
            }
            // Global resources do not have initializers. Function locals
            // are handled by offline validation, not interface reflection.
            if ((args[2] != 7 && args.size() != 3) || (args[2] == 7) != in_function_) {
                return false;
            }
            const std::array values{args[0], args[2]};
            return record(opcode, args[1], values);
        } else if (opcode == 71) {
            return args.size() >= 2 && valid_id(args[0]) &&
                   decorate(nodes_[args[0]].decoration, args[1], args.subspan(2), false);
        } else if (opcode == 72) {
            const auto member_count = interface_ == ShaderInterface::scene_unlit ? 2U : 1U;
            return args.size() >= 3 && valid_id(args[0]) && args[1] < member_count &&
                   decorate(nodes_[args[0]].members[args[1]], args[2], args.subspan(3), true);
        } else if (opcode >= 73 && opcode <= 75) {
            // Decoration groups could hide a layout change from this reader.
            return false;
        }
        return true;
    }

    const Node* node(std::uint32_t id, std::uint32_t opcode, std::size_t size) const {
        if (!valid_id(id)) {
            return nullptr;
        }
        const auto& result = nodes_[id];
        return result.opcode == opcode && result.operands.size() == size ? &result : nullptr;
    }

    bool float32(std::uint32_t id) const {
        const auto* type = node(id, 22, 1);
        return type != nullptr && type->operands[0] == 32;
    }

    bool float_vector(std::uint32_t id, std::uint32_t width) const {
        const auto* type = node(id, 23, 2);
        return type != nullptr && type->operands[1] == width && float32(type->operands[0]);
    }

    bool globals(std::uint32_t id) const {
        const bool scene = interface_ == ShaderInterface::scene_unlit;
        const auto* structure = node(id, 30, scene ? 2 : 1);
        if (structure == nullptr || !structure->decoration.block ||
            structure->decoration.unsupported) {
            return false;
        }
        const auto& transform = structure->members[0];
        if (interface_ == ShaderInterface::gui)
            return !transform.unsupported && transform.offset == 0 && !transform.stride &&
                   !transform.row_major && float_vector(structure->operands[0], 4);
        if (transform.unsupported || transform.offset != 0 || transform.stride != 16 ||
            !transform.row_major) {
            return false;
        }
        if (scene) {
            const auto& factor = structure->members[1];
            if (factor.unsupported || factor.offset != 64 || factor.stride || factor.row_major ||
                !float_vector(structure->operands[1], 4)) {
                return false;
            }
        }
        const auto* matrix = node(structure->operands[0], 24, 2);
        return matrix != nullptr && matrix->operands[1] == 4 &&
               float_vector(matrix->operands[0], 4);
    }

    bool texture(std::uint32_t id) const {
        const auto* type = node(id, 25, 7);
        if (type == nullptr || !float32(type->operands[0])) {
            return false;
        }
        const std::array<std::uint32_t, 6> expected{1, 2, 0, 0, 1, 0};
        return std::equal(expected.begin(), expected.end(), type->operands.begin() + 1);
    }

    Result<void> validate_variables() const {
        const bool vertex = artifact_.stage == ShaderStage::vertex;
        const bool scene = interface_ == ShaderInterface::scene_unlit;
        const bool gui = interface_ == ShaderInterface::gui;
        std::set<std::uint32_t> inputs;
        std::set<std::uint32_t> outputs;
        std::set<std::uint32_t> bindings;
        std::set<std::uint32_t> globals_seen;
        std::uint32_t positions = 0;
        for (std::size_t id = 1; id < nodes_.size(); ++id) {
            const auto& variable = nodes_[id];
            if (variable.opcode != 59 || variable.operands[1] == 7) {
                continue;
            }
            const auto storage = variable.operands[1];
            const auto* pointer = node(variable.operands[0], 32, 2);
            if (pointer == nullptr || pointer->operands[0] != storage ||
                variable.decoration.unsupported) {
                return resource_error("Shader variable has an invalid pointer or decoration",
                                      "shader.layout");
            }
            globals_seen.insert(static_cast<std::uint32_t>(id));
            const auto type = pointer->operands[1];
            const auto& decoration = variable.decoration;
            if (storage == 1 || storage == 3) { // Input or Output.
                if (decoration.binding || decoration.descriptor_set || decoration.block) {
                    return resource_error("Shader input/output cannot be a descriptor",
                                          "shader.layout");
                }
                if (decoration.builtin) {
                    if (!vertex || storage != 3 || decoration.builtin != 0 || decoration.location ||
                        !float_vector(type, 4) || ++positions != 1) {
                        return resource_error("Unsupported shader builtin", "shader.layout");
                    }
                } else {
                    if (!decoration.location) {
                        return resource_error("Shader input/output location is missing",
                                              "shader.layout");
                    }
                    const auto location = *decoration.location;
                    auto& locations = storage == 1 ? inputs : outputs;
                    const auto width =
                        gui                      ? ((vertex && storage == 1)
                                                        ? (location == 2 ? 4U : 2U)
                                                        : ((!vertex && storage == 3) || location == 1 ? 4U : 2U))
                        : vertex && storage == 1 ? (location == 0 ? 3U : (scene ? 4U : 2U))
                                                 : (scene || (!vertex && storage == 3) ? 4U : 2U);
                    if (!locations.insert(location).second || !float_vector(type, width)) {
                        return resource_error("Shader input/output type or location mismatch",
                                              "shader.layout");
                    }
                }
            } else if (storage == 0 || storage == 2) { // UniformConstant or Uniform.
                if (decoration.location || decoration.builtin || decoration.block ||
                    decoration.descriptor_set != 0 || !decoration.binding ||
                    !bindings.insert(*decoration.binding).second) {
                    return resource_error("Shader descriptor set/binding mismatch",
                                          "shader.layout");
                }
                const auto binding = *decoration.binding;
                const bool valid = vertex ? storage == 2 && binding == 256 && globals(type)
                                          : !scene && storage == 0 &&
                                                ((binding == 0 && texture(type)) ||
                                                 (binding == 128 && node(type, 26, 0) != nullptr));
                if (!valid) {
                    return resource_error(
                        "Shader descriptor type or constant-buffer layout mismatch",
                        "shader.layout");
                }
            } else {
                return resource_error("Unsupported global shader storage class", "shader.layout");
            }
        }
        const std::set<std::uint32_t> expected_inputs =
            gui ? (vertex ? std::set<std::uint32_t>{0, 1, 2} : std::set<std::uint32_t>{0, 1})
                : (vertex ? std::set<std::uint32_t>{0, 1} : std::set<std::uint32_t>{0});
        const std::set<std::uint32_t> expected_outputs =
            gui && vertex ? std::set<std::uint32_t>{0, 1} : std::set<std::uint32_t>{0};
        const std::set<std::uint32_t> expected_bindings =
            vertex ? std::set<std::uint32_t>{256}
                   : (scene ? std::set<std::uint32_t>{} : std::set<std::uint32_t>{0, 128});
        if (inputs != expected_inputs || outputs != expected_outputs ||
            bindings != expected_bindings || positions != (vertex ? 1U : 0U) ||
            globals_seen != entry_interfaces_) {
            return resource_error("Shader stage interface does not match the selected layout",
                                  "shader.layout");
        }
        return {};
    }

    const ShaderArtifact& artifact_;
    ShaderInterface interface_;
    std::vector<Node> nodes_;
    std::set<std::uint32_t> entry_interfaces_;
    bool entry_seen_{false};
    bool memory_model_seen_{false};
    bool shader_capability_seen_{false};
    std::uint32_t entry_function_{0};
    bool entry_function_seen_{false};
    bool in_function_{false};
};

bool valid_extent(WindowExtent extent) {
    return extent.width > 0 && extent.height > 0 && extent.width <= 1024 && extent.height <= 1024;
}

} // namespace

Result<void> validate_shader_artifact(const ShaderArtifact& artifact,
                                      ShaderInterface shader_interface) {
    return InterfaceReader(artifact, shader_interface).validate();
}

Result<ShaderArtifact> decode_shader_artifact(std::span<const std::byte> bytes, ShaderStage stage,
                                              ShaderInterface shader_interface) {
    if (bytes.size() < 20 || bytes.size() > max_shader_bytes || bytes.size() % 4 != 0) {
        return resource_error(
            "Shader artifact must contain 20..4194304 bytes aligned to four bytes");
    }
    const auto entry = shader_interface == ShaderInterface::scene_unlit
                           ? (stage == ShaderStage::vertex ? "vsScene" : "psScene")
                       : shader_interface == ShaderInterface::gui
                           ? (stage == ShaderStage::vertex ? "vsGui" : "psGui")
                           : (stage == ShaderStage::vertex ? "vsMain" : "psMain");
    ShaderArtifact artifact{stage, entry, {}};
    artifact.words.reserve(bytes.size() / 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
        std::uint32_t value = 0;
        for (std::uint32_t index = 0; index < 4; ++index) {
            value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8);
        }
        artifact.words.push_back(value);
    }
    auto valid = validate_shader_artifact(artifact, shader_interface);
    if (!valid) {
        return valid.error();
    }
    return artifact;
}

Result<ShaderArtifact> load_shader_artifact(const std::filesystem::path& path, ShaderStage stage,
                                            ShaderInterface shader_interface) {
    auto path_text = detail::artifact_path_text(path);
    if (!path_text) {
        return path_text.error();
    }
    const auto fail = [&path_text](std::string message, ErrorCode code) -> Result<ShaderArtifact> {
        return Error{code, std::move(message), {"shader.load", {}, path_text.value()}};
    };
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(path, status_error)) {
        return fail("Shader artifact must be an accessible regular file", ErrorCode::io_failure);
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return fail("Cannot open shader artifact", ErrorCode::io_failure);
    }
    const auto size = stream.tellg();
    if (size < 20 || size > static_cast<std::streamoff>(max_shader_bytes) || size % 4 != 0) {
        return fail("Shader artifact has an unsupported byte length", ErrorCode::invalid_argument);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
        stream.peek() != std::char_traits<char>::eof()) {
        return fail("Cannot read a stable complete shader artifact", ErrorCode::io_failure);
    }
    auto artifact = decode_shader_artifact(bytes, stage, shader_interface);
    if (!artifact) {
        auto error = artifact.error();
        error.context.path = path_text.value();
        return error;
    }
    return artifact;
}

Result<void> validate_offscreen_request(const OffscreenRequest& request) {
    const auto fail = [](std::string message) -> Result<void> {
        return resource_error(std::move(message), "graphics.offscreen.validate");
    };
    if (!valid_extent(request.extent) || !valid_extent(request.texture_extent)) {
        return fail("Offscreen and texture dimensions must be in 1..1024");
    }
    const auto texture_bytes =
        static_cast<std::size_t>(request.texture_extent.width) * request.texture_extent.height * 4;
    if (request.texture_rgba8.size() != texture_bytes) {
        return fail("RGBA8 upload storage must exactly match its tightly packed extent");
    }
    if (request.vertices.size() < 3 || request.vertices.size() > 65536 ||
        request.indices.size() < 3 || request.indices.size() > 196608 ||
        request.indices.size() % 3 != 0) {
        return fail("Offscreen geometry exceeds the bounded triangle-list contract");
    }
    for (const auto& vertex : request.vertices) {
        if (!std::ranges::all_of(vertex.position,
                                 [](float value) { return std::isfinite(value); }) ||
            !std::ranges::all_of(vertex.uv, [](float value) { return std::isfinite(value); })) {
            return fail("Offscreen vertex values must be finite");
        }
    }
    if (!std::ranges::all_of(request.transform, [](float value) { return std::isfinite(value); })) {
        return fail("Offscreen transform values must be finite");
    }
    for (const auto index : request.indices) {
        if (index >= request.vertices.size()) {
            return fail("Offscreen index references a vertex outside the upload");
        }
    }
    if (request.vertex_shader.stage != ShaderStage::vertex ||
        request.pixel_shader.stage != ShaderStage::pixel) {
        return fail("Offscreen shader stages do not match the pipeline");
    }
    auto vertex = validate_shader_artifact(request.vertex_shader);
    if (!vertex) {
        return vertex.error();
    }
    return validate_shader_artifact(request.pixel_shader);
}

} // namespace shark::client
