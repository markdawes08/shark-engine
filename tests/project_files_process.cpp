#include <shark/tools/project_files.hpp>

#include "project_files_native.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T require(Result<T> value) {
    if (!value) {
        throw std::runtime_error{std::string{error_code_name(value.error().code)} + ":" +
                                 value.error().context.operation};
    }
    return std::move(value).value();
}

template <typename Id> Id id(unsigned value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = "0123456789abcdef"[value];
    return Id{require(StableId::parse(text))};
}

ProjectDocument project() {
    return {id<ProjectId>(1),
            id<WorldId>(2),
            {"Process project"},
            id<SceneId>(3),
            {{id<SceneId>(3), require(ProjectPath::parse("scene.sharkscene"))}},
            "{}"};
}

SceneDocument scene(std::string name) {
    return {id<ProjectId>(1),
            id<SceneId>(3),
            std::nullopt,
            {{id<AuthoredEntityId>(4), std::nullopt, {std::move(name)}, {}, "{}"}},
            "{}"};
}

detail::FilePhase phase(std::string_view value) {
    const std::array entries{std::pair{"staged", detail::FilePhase::staged},
                             std::pair{"written", detail::FilePhase::written},
                             std::pair{"flushed", detail::FilePhase::flushed},
                             std::pair{"before_publish", detail::FilePhase::before_publish},
                             std::pair{"published", detail::FilePhase::published}};
    for (const auto& [name, selected] : entries) {
        if (value == name) {
            return selected;
        }
    }
    throw std::runtime_error{"unknown interruption phase"};
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4) {
            throw std::runtime_error{"expected mode root [phase]"};
        }
        const std::string_view mode{argv[1]};
        const std::string root_text{argv[2]};
        const std::u8string root_utf8{root_text.begin(), root_text.end()};
        auto opening = ProjectFiles::open(std::filesystem::path{root_utf8});
        if (mode == "check-busy") {
            if (opening || opening.error().code != ErrorCode::conflict) {
                throw std::runtime_error{"second writer was not rejected"};
            }
            std::cout << "{\"status\":\"PASS\",\"state\":\"busy\"}\n";
            return 0;
        }
        auto files = require(std::move(opening));
        if (mode == "seed" || mode == "seed-new" || mode == "seed-recovered") {
            require(files->write_project(require(ProjectPath::parse("project.sharkproject")),
                                         project(), std::nullopt));
            const auto name = mode == "seed"       ? "Before"
                              : mode == "seed-new" ? "After"
                                                   : "Recovered";
            require(files->write_scene(project(), scene(name), std::nullopt));
        } else if (mode == "hold") {
            std::cout << "{\"status\":\"PASS\",\"state\":\"ready\"}\n" << std::flush;
            std::string release;
            if (!std::getline(std::cin, release) || release != "release") {
                throw std::runtime_error{"holder did not receive release"};
            }
        } else if (mode == "abort-lock") {
            std::_Exit(73);
        } else if (mode == "save-exit") {
            if (argc != 4) {
                throw std::runtime_error{"missing interruption phase"};
            }
            auto selected = phase(argv[3]);
            const auto loaded_project =
                require(files->read_project(require(ProjectPath::parse("project.sharkproject"))));
            auto loaded =
                require(files->load_scene(loaded_project.source.document, id<SceneId>(3)));
            require(
                require(loaded.owner->execute(SetNameCommand{id<AuthoredEntityId>(4), {"After"}},
                                              require(loaded.owner->status()).revision))
                    .outcome);
            const auto status = require(loaded.owner->status());
            if (!status.dirty) {
                throw std::runtime_error{"interrupted save did not start with an edited owner"};
            }
            const auto hook = [](detail::FilePhase reached, void* context) noexcept {
                if (reached == *static_cast<detail::FilePhase*>(context)) {
                    std::_Exit(73);
                }
            };
            static_cast<void>(
                detail::set_file_test_control({detail::FileFault::none, hook, &selected}));
            require(files->save_scene(*loaded.owner, status.revision, loaded.file_revision));
            throw std::runtime_error{"interruption phase was never reached"};
        } else if (mode == "inspect") {
            if (argc != 4) {
                throw std::runtime_error{"missing expected persisted name"};
            }
            const auto loaded_project =
                require(files->read_project(require(ProjectPath::parse("project.sharkproject"))));
            const auto loaded =
                require(files->load_scene(loaded_project.source.document, id<SceneId>(3)));
            const auto captured = require(loaded.owner->capture());
            if (captured.project != project() || captured.scene != scene(argv[3]) ||
                captured.dirty || loaded.source_schema_version != source_document_version ||
                loaded_project.source.source_schema_version != source_document_version) {
                throw std::runtime_error{"persisted scene activation differed from expected state"};
            }
        } else if (mode == "recover") {
            const auto loaded_project =
                require(files->read_project(require(ProjectPath::parse("project.sharkproject"))));
            auto loaded =
                require(files->load_scene(loaded_project.source.document, id<SceneId>(3)));
            require(require(loaded.owner->execute(
                                SetNameCommand{id<AuthoredEntityId>(4), {"Recovered"}},
                                require(loaded.owner->status()).revision))
                        .outcome);
            const auto saved = require(files->save_scene(
                *loaded.owner, require(loaded.owner->status()).revision, loaded.file_revision));
            if (!saved.current_scene_clean || require(loaded.owner->status()).dirty) {
                throw std::runtime_error{"recovered save did not acknowledge the current revision"};
            }
        } else {
            throw std::runtime_error{"unknown mode"};
        }
        std::cout << "{\"status\":\"PASS\"}\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    } catch (...) {
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
