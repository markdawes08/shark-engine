#include "shark/script_components.hpp"

#include "shark/project_path.hpp"

#include "utf8.hpp"

#include <string_view>
#include <utility>

namespace shark {

Result<Script> validate_script(Script script) {
    constexpr std::string_view root{"scripts/"};
    constexpr std::string_view suffix{".lua"};
    const auto source = std::string_view{script.source};
    const auto last_slash = source.find_last_of('/');
    if (source.size() < script_minimum_source_path_bytes ||
        source.size() > script_maximum_source_path_bytes || !detail::valid_utf8(source, true) ||
        !source.starts_with(root) || !source.ends_with(suffix) ||
        last_slash == std::string_view::npos || source.size() - last_slash <= suffix.size() + 1) {
        return Error{ErrorCode::invalid_path,
                     "Script source must be a project path under scripts/ with a .lua filename",
                     {.operation = "script.validate"}};
    }
    auto path = ProjectPath::parse(source, {.operation = "script.validate"});
    if (!path)
        return std::move(path).error();
    return script;
}

} // namespace shark
