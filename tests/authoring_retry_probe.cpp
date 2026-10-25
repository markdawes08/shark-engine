#include <shark/tools/project_files.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation};
    return std::move(result).value();
}
void check(bool condition) {
    if (!condition)
        throw std::runtime_error{"retry process invariant failed"};
}
template <typename T> T id(unsigned value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = "0123456789abcdef"[value];
    return T{take(StableId::parse(text))};
}
ProjectDocument project() {
    return {id<ProjectId>(1),
            id<WorldId>(2),
            {"Retry process"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("scene.sharkscene"))}},
            "{}"};
}
SceneDocument scene() {
    return {id<ProjectId>(1),
            id<SceneId>(3),
            std::nullopt,
            {{id<AuthoredEntityId>(4), std::nullopt, {"Before"}, {}, "{}"}},
            "{}"};
}
SceneRevision revision(const AuthoredScene& owner) {
    return take(owner.status()).revision;
}
AuthoringCommand rename(std::string name) {
    return SetNameCommand{id<AuthoredEntityId>(4), {std::move(name)}};
}
std::string name(const AuthoredScene& owner) {
    return take(owner.capture()).scene.entities.at(0).name.value;
}
AuthoringMutationId read_intent(const std::filesystem::path& root) {
    std::ifstream input{root / "lost-intent.txt"};
    std::string version, scope, sequence, extra;
    check(static_cast<bool>(input >> version >> scope >> sequence));
    check(version == "retry-fixture-1" && scope.size() == 36 && sequence == "1");
    check(!(input >> extra));
    return {AuthoringMutationScope{take(StableId::parse(scope))}, 1};
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3);
        const std::string_view mode{argv[1]};
        const std::string text{argv[2]};
        const std::filesystem::path root{std::u8string{text.begin(), text.end()}};
        auto files = take(ProjectFiles::open(root));
        if (mode == "produce") {
            take(files->write_project(take(ProjectPath::parse("project.sharkproject")), project(),
                                      std::nullopt));
            auto owner = take(AuthoredScene::create(project(), scene()));
            const auto state = take(owner->mutation_status());
            check(state.next.has_value());
            const auto mutation = *state.next;
            // Persist caller intent before dispatch. The scene save below is an
            // independent, explicit persistence action, not a durable retry log.
            {
                std::ofstream intent{root / "lost-intent.txt", std::ios::binary};
                intent << "retry-fixture-1\n" << mutation.scope.value().to_string() << "\n1\n";
                intent.flush();
                check(static_cast<bool>(intent));
            }
            {
                auto reply =
                    take(owner->mutate({mutation, state.revision, rename("Original lost reply")}));
                check(reply.state == AuthoringMutationState::recorded && reply.outcome &&
                      reply.outcome.value()->result);
                // Drop the reply; the recovering caller retains only the intent.
            }
            take(files->save_scene(*owner, revision(*owner), std::nullopt));
            check(name(*owner) == "Original lost reply");
            check(take(owner->mutation_status()).high_watermark == 1 &&
                  take(owner->history()).undo_entries == 1);
            std::cout << "{\"status\":\"PASS\",\"phase\":\"produce\",\"scope\":\""
                      << mutation.scope.value().to_string()
                      << "\",\"high_watermark\":1,\"history_entries\":1,\"name\":\"Original lost "
                         "reply\"}\n";
        } else if (mode == "recover") {
            const auto old = read_intent(root);
            auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
            auto& owner = *loaded.owner;
            check(name(owner) == "Original lost reply");
            const auto state = take(owner.mutation_status());
            check(state.scope != old.scope && state.high_watermark == 0 && state.next.has_value());
            auto intervening =
                take(owner.execute(rename("Intervening after restart"), revision(owner)));
            check(static_cast<bool>(intervening.outcome));
            const auto before = take(owner.capture());
            const auto ticket = take(owner.history()).next_undo->ticket;
            auto query = take(owner.mutation_outcome(old));
            check(query.state == AuthoringMutationState::unknown_outcome && !query.outcome);
            // Even refreshing the expected revision cannot authorize the old ID.
            auto retry = take(owner.mutate({old, before.revision, rename("Original lost reply")}));
            check(retry.state == AuthoringMutationState::unknown_outcome && !retry.outcome);
            check(take(owner.capture()).scene == before.scene &&
                  revision(owner) == before.revision);
            check(take(owner.history()).next_undo->ticket == ticket &&
                  take(owner.mutation_status()).high_watermark == 0);
            // After inspecting actual state, issue a new intent, not a blind retry.
            const AuthoringMutationRequest reconciled{*state.next, before.revision,
                                                      rename("Reconciled new intent")};
            auto applied = take(owner.mutate(reconciled));
            check(applied.state == AuthoringMutationState::recorded && applied.outcome &&
                  applied.outcome.value()->result);
            const auto after = revision(owner);
            auto replay = take(owner.mutate(reconciled));
            check(replay.state == AuthoringMutationState::replayed &&
                  replay.outcome.value() == applied.outcome.value());
            check(revision(owner) == after && name(owner) == "Reconciled new intent");
            take(files->save_scene(owner, after, loaded.file_revision));
            check(take(owner.history()).undo_entries == 2 &&
                  take(owner.mutation_status()).high_watermark == 1);
            std::cout << "{\"status\":\"PASS\",\"phase\":\"recover\",\"scope\":\""
                      << state.scope.value().to_string() << "\",\"old_scope\":\""
                      << old.scope.value().to_string()
                      << "\",\"query\":\"unknown_outcome\",\"retry\":\"unknown_outcome\",\"high_"
                         "watermark\":1,"
                         "\"history_entries\":2,\"name\":\"Reconciled new intent\"}\n";
        } else if (mode == "verify") {
            const auto old = read_intent(root);
            auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
            const auto state = take(loaded.owner->mutation_status());
            check(state.scope != old.scope && state.high_watermark == 0);
            check(name(*loaded.owner) == "Reconciled new intent" &&
                  take(loaded.owner->history()).undo_entries == 0);
            check(take(loaded.owner->mutation_outcome(old)).state ==
                  AuthoringMutationState::unknown_outcome);
            std::cout << "{\"status\":\"PASS\",\"phase\":\"verify\",\"scope\":\""
                      << state.scope.value().to_string()
                      << "\",\"high_watermark\":0,\"history_entries\":0,\"name\":\"Reconciled new "
                         "intent\"}\n";
        } else {
            throw std::runtime_error{"unknown retry fixture mode"};
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
