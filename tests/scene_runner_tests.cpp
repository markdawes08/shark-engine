#include <shark/tools/scene_runner.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(char digit) {
    std::string value = "20000000-0000-4000-8000-000000000001";
    value.back() = digit;
    return T{take(StableId::parse(value))};
}
struct Fixture {
    ProjectDocument project{id<ProjectId>('1'),
                            id<WorldId>('2'),
                            Name{"Runner tests"},
                            id<SceneId>('3'),
                            {{id<SceneId>('3'), take(ProjectPath::parse("test.sharkscene"))}}};
    SceneDocument scene{project.project, project.startup_scene, {}, {}};
    SceneRunConfig config;
    SceneRunReport run(std::span<const SceneRunCase> cases, std::stop_token stop = {}) {
        return take(run_scene_cases(project, scene, config, cases, stop));
    }
};
SceneRunCase passing(std::string name = "pass") {
    return {.id = std::move(name),
            .setup = {},
            .tick = [](SceneRunContext& context,
                       const SceneRunTick&) { return context.require(true, "A passing tick"); },
            .finish = {}};
}
} // namespace

TEST_CASE("Runner admits exact rational ticks and delivers ordered one-shot inputs",
          "[scene-runner]") {
    for (const std::uint32_t rate : {1U, 20U, 59U, 60U, 1000U}) {
        Fixture fixture;
        fixture.config.ticks = 7;
        fixture.config.tick_rate_hz = rate;
        fixture.config.inputs = {
            {0, "first", 5}, {3, "second", -7}, {3, "third", 9}, {6, "last", 1}};
        std::vector<std::uint64_t> ticks;
        std::vector<SceneRunInput> delivered;
        auto test = passing();
        test.tick = [&](SceneRunContext& context, const SceneRunTick& step) {
            ticks.push_back(step.index);
            delivered.insert(delivered.end(), step.inputs.begin(), step.inputs.end());
            return context.require(step.tick_rate_hz == rate, "Configured rational rate");
        };
        const std::array cases{test};
        auto report = fixture.run(cases);
        CHECK(report.status == SceneRunStatus::passed);
        CHECK(ticks == std::vector<std::uint64_t>{0, 1, 2, 3, 4, 5, 6});
        CHECK(delivered == fixture.config.inputs);
        CHECK(report.cases.front().ticks_completed == 7);
        CHECK(report.cases.front().assertions == 7);
    }
}

TEST_CASE("Runner RNG has fixed vectors and resets independently for every case",
          "[scene-runner]") {
    Fixture fixture;
    fixture.config.seed = 0;
    fixture.config.ticks = 2;
    std::vector<std::uint64_t> values;
    auto test = passing("first");
    test.tick = [&](SceneRunContext& context, const SceneRunTick&) {
        values.push_back(take(context.random_u64()));
        return context.require(true, "Recorded random data");
    };
    auto second = test;
    second.id = "second";
    const std::array cases{test, second};
    auto report = fixture.run(cases);
    REQUIRE(report.status == SceneRunStatus::passed);
    CHECK(values == std::vector<std::uint64_t>{0xe220a8397b1dcdafULL, 0x6e789e6aa1b965f4ULL,
                                               0xe220a8397b1dcdafULL, 0x6e789e6aa1b965f4ULL});
    CHECK(report.cases[0].random_draws == 2);
    fixture.config.seed = std::numeric_limits<std::uint64_t>::max();
    values.clear();
    CHECK(fixture.run(cases).status == SceneRunStatus::passed);
    CHECK(values.front() != 0xe220a8397b1dcdafULL);
}

TEST_CASE("One case shares its authoring owner and subsequent cases start from source",
          "[scene-runner]") {
    Fixture fixture;
    fixture.config.ticks = 3;
    const auto entity = id<AuthoredEntityId>('4');
    auto test = passing("edit");
    test.setup = [entity](SceneRunContext& context) -> Result<void> {
        auto status = take(context.owner().status());
        auto reply = take(context.owner().execute(CreateEntityCommand{entity}, status.revision));
        REQUIRE(reply.outcome);
        return context.require(reply.outcome.value().changed, "Created entity", entity);
    };
    test.tick = [entity](SceneRunContext& context, const SceneRunTick& tick) -> Result<void> {
        Transform value;
        value.translation.x = static_cast<double>(tick.index + 1);
        auto status = take(context.owner().status());
        auto reply =
            take(context.owner().execute(SetTransformCommand{entity, value}, status.revision));
        REQUIRE(reply.outcome);
        auto capture = take(context.owner().capture(reply.observed_revision));
        return context.require(capture.scene.entities.front().transform == value,
                               "Shared scene state", entity);
    };
    auto fresh = passing("fresh");
    fresh.setup = [](SceneRunContext& context) {
        return context.require(take(context.owner().capture()).scene.entities.empty(),
                               "Fresh source baseline");
    };
    const std::array cases{test, fresh};
    auto report = fixture.run(cases);
    REQUIRE(report.status == SceneRunStatus::passed);
    REQUIRE(report.cases[0].initial_revision);
    REQUIRE(report.cases[0].final_revision);
    CHECK(report.cases[0].final_revision->revision ==
          report.cases[0].initial_revision->revision + 4);
    CHECK(report.cases[0].initial_revision->scene_instance !=
          report.cases[1].initial_revision->scene_instance);
    CHECK(fixture.scene.entities.empty());
}

TEST_CASE("Ignored scene assertions remain terminal with entity and tick context",
          "[scene-runner]") {
    Fixture fixture;
    bool finished = false;
    std::uint32_t calls = 0;
    auto test = passing("broken");
    test.tick = [&](SceneRunContext& context, const SceneRunTick& tick) -> Result<void> {
        ++calls;
        static_cast<void>(context.require(tick.index != 3, "Expected entity pose \xE6\xB5\xB7",
                                          id<AuthoredEntityId>('4')));
        return {};
    };
    test.finish = [&](SceneRunContext&) -> Result<void> {
        finished = true;
        return {};
    };
    const std::array cases{test, passing("following")};
    auto report = fixture.run(cases);
    CHECK(report.status == SceneRunStatus::failed);
    CHECK(report.cases[1].status == SceneRunStatus::passed);
    CHECK(calls == 4);
    CHECK_FALSE(finished);
    const auto& result = report.cases.front();
    REQUIRE(result.failure);
    CHECK(result.ticks_completed == 3);
    CHECK(result.failure->kind == SceneRunFailureKind::assertion);
    CHECK(result.failure->phase == SceneRunPhase::tick);
    CHECK(result.failure->tick == 3);
    CHECK(result.failure->error.context.entity_id == id<AuthoredEntityId>('4').value().to_string());
    auto encoded = take(encode_scene_run_report(report));
    CHECK(encoded.find("\"renderer_initialized\": false") != std::string::npos);
    CHECK(encoded.find("\"tick\": 3") != std::string::npos);
}

TEST_CASE("Runner catches callback errors and exceptions in every phase", "[scene-runner]") {
    for (auto phase : {SceneRunPhase::setup, SceneRunPhase::tick, SceneRunPhase::finish}) {
        for (bool throws : {false, true}) {
            Fixture fixture;
            fixture.config.ticks = 2;
            auto failure = [throws](SceneRunContext&) -> Result<void> {
                if (throws)
                    throw std::runtime_error{"unbounded untrusted exception text"};
                return Error{ErrorCode::conflict,
                             "Controlled callback error",
                             {.operation = "fixture.callback"}};
            };
            auto test = passing();
            if (phase == SceneRunPhase::setup)
                test.setup = failure;
            if (phase == SceneRunPhase::tick)
                test.tick = [failure](SceneRunContext& context, const SceneRunTick&) {
                    return failure(context);
                };
            if (phase == SceneRunPhase::finish)
                test.finish = failure;
            const std::array cases{test};
            auto report = fixture.run(cases);
            REQUIRE(report.status == SceneRunStatus::failed);
            const auto& error = report.cases.front().failure;
            REQUIRE(error);
            CHECK(error->phase == phase);
            CHECK(error->kind ==
                  (throws ? SceneRunFailureKind::exception : SceneRunFailureKind::callback));
            CHECK(error->error.message.find("untrusted") == std::string::npos);
        }
    }
}

TEST_CASE("Case assertions and random work have hard bounds", "[scene-runner]") {
    for (bool random : {false, true}) {
        Fixture fixture;
        auto test = passing();
        test.tick = [random](SceneRunContext& context, const SceneRunTick&) -> Result<void> {
            const auto maximum =
                random ? scene_run_maximum_random_draws : scene_run_maximum_assertions;
            for (std::uint32_t index = 0; index <= maximum; ++index) {
                if (random)
                    static_cast<void>(context.random_u64());
                else
                    static_cast<void>(context.require(true, "Bounded assertion"));
            }
            return {};
        };
        const std::array cases{test};
        auto report = fixture.run(cases);
        REQUIRE(report.cases.front().failure);
        CHECK(report.cases.front().failure->kind == SceneRunFailureKind::limit);
        CHECK(report.cases.front().ticks_completed == 0);
    }
}

TEST_CASE("Cancelled runs preserve completed work and leave later cases not run",
          "[scene-runner]") {
    for (bool before : {false, true}) {
        Fixture fixture;
        std::stop_source stop;
        if (before)
            stop.request_stop();
        auto test = passing();
        test.tick = [&](SceneRunContext& context, const SceneRunTick&) {
            stop.request_stop();
            return context.require(true, "Completed before observing cancellation");
        };
        const std::array cases{test, passing("later")};
        auto report = fixture.run(cases, stop.get_token());
        CHECK(report.status == SceneRunStatus::cancelled);
        CHECK(report.cases.front().ticks_completed == (before ? 0U : 1U));
        CHECK(report.cases.front().status ==
              (before ? SceneRunStatus::not_run : SceneRunStatus::cancelled));
        CHECK(report.cases.back().status == SceneRunStatus::not_run);
        CHECK(encode_scene_run_report(report));
    }
}

TEST_CASE("Runner contexts reject worker use without changing owner counters", "[scene-runner]") {
    Fixture fixture;
    fixture.config.ticks = 1;
    auto test = passing();
    test.tick = [](SceneRunContext& context, const SceneRunTick&) {
        bool rejected = false;
        std::thread worker{[&] {
            const auto assertion = context.require(true, "Worker assertion");
            const auto random = context.random_u64();
            rejected = !assertion && !random && assertion.error().code == ErrorCode::conflict &&
                       random.error().code == ErrorCode::conflict;
        }};
        worker.join();
        return context.require(rejected, "Owner thread enforced");
    };
    const std::array cases{test};
    const auto report = fixture.run(cases);
    CHECK(report.status == SceneRunStatus::passed);
    CHECK(report.cases.front().assertions == 1);
    CHECK(report.cases.front().random_draws == 0);
}

TEST_CASE("Runner validates every configuration and case before invoking any callback",
          "[scene-runner]") {
    Fixture fixture;
    std::size_t calls = 0;
    auto test = passing();
    test.setup = [&](SceneRunContext&) -> Result<void> {
        ++calls;
        return {};
    };
    const std::array cases{test};
    const auto invalid = [&](SceneRunConfig config) {
        auto result = run_scene_cases(fixture.project, fixture.scene, config, cases);
        CHECK_FALSE(result);
        CHECK(calls == 0);
    };
    auto config = fixture.config;
    config.ticks = 0;
    invalid(config);
    config.ticks = scene_run_maximum_ticks + 1;
    invalid(config);
    config = fixture.config;
    config.tick_rate_hz = 0;
    invalid(config);
    config.tick_rate_hz = 1001;
    invalid(config);
    config = fixture.config;
    config.run_id = "bad/id";
    invalid(config);
    config = fixture.config;
    config.revision.assign(65, 'a');
    invalid(config);
    config = fixture.config;
    config.inputs = {{2, "a", 0}, {1, "b", 0}};
    invalid(config);
    config.inputs = {{60, "a", 0}};
    invalid(config);
    config.inputs.assign(scene_run_maximum_inputs + 1, {0, "a", 0});
    invalid(config);
    const std::array duplicate{test, test};
    CHECK_FALSE(run_scene_cases(fixture.project, fixture.scene, fixture.config, duplicate));
    CHECK_FALSE(run_scene_cases(fixture.project, fixture.scene, fixture.config, {}));
    auto missing = test;
    missing.tick = {};
    const std::array incomplete{test, missing};
    CHECK_FALSE(run_scene_cases(fixture.project, fixture.scene, fixture.config, incomplete));
    fixture.scene.project = id<ProjectId>('5');
    CHECK_FALSE(run_scene_cases(fixture.project, fixture.scene, fixture.config, cases));
    CHECK(calls == 0);
}

TEST_CASE("Empty or malformed diagnostics cannot manufacture a passing run", "[scene-runner]") {
    for (const std::string& message :
         {std::string{}, std::string(1025, 'x'), std::string{"\xff"}}) {
        Fixture fixture;
        auto test = passing();
        test.tick = [&](SceneRunContext& context, const SceneRunTick&) {
            return context.require(true, message);
        };
        const std::array cases{test};
        auto report = fixture.run(cases);
        CHECK(report.status == SceneRunStatus::failed);
        CHECK(encode_scene_run_report(report));
    }
    Fixture fixture;
    auto test = passing();
    test.tick = [](SceneRunContext&, const SceneRunTick&) -> Result<void> { return {}; };
    const std::array cases{test};
    auto report = fixture.run(cases);
    CHECK(report.status == SceneRunStatus::failed);
    report.status = SceneRunStatus::passed;
    CHECK_FALSE(encode_scene_run_report(report));
}

TEST_CASE("Runner supports its maximum tick and case counts with explicit assertions",
          "[scene-runner]") {
    Fixture fixture;
    fixture.config.ticks = scene_run_maximum_ticks;
    auto test = passing();
    test.tick = [](SceneRunContext&, const SceneRunTick&) -> Result<void> { return {}; };
    test.finish = [](SceneRunContext& context) {
        return context.require(true, "All scheduled ticks completed");
    };
    const std::array cases{test};
    auto report = fixture.run(cases);
    CHECK(report.status == SceneRunStatus::passed);
    CHECK(report.cases.front().ticks_completed == scene_run_maximum_ticks);
    fixture.config.ticks = 1;
    std::vector<SceneRunCase> maximum;
    for (std::size_t index = 0; index < scene_run_maximum_cases; ++index)
        maximum.push_back(passing("case-" + std::to_string(index)));
    CHECK(fixture.run(maximum).cases.size() == scene_run_maximum_cases);
    maximum.push_back(passing("excess"));
    CHECK_FALSE(run_scene_cases(fixture.project, fixture.scene, fixture.config, maximum));
}

TEST_CASE("Result encoder rejects contradictory counters identities and failure locations",
          "[scene-runner]") {
    Fixture fixture;
    fixture.config.ticks = 1;
    const std::array cases{passing()};
    const auto valid = fixture.run(cases);
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        auto report = valid;
        auto& result = report.cases.front();
        switch (mutation) {
        case 0:
            result.status = static_cast<SceneRunStatus>(999);
            break;
        case 1:
            result.ticks_completed = 2;
            break;
        case 2:
            result.final_revision.reset();
            break;
        case 3:
            result.initial_revision->revision = 2;
            result.final_revision->revision = 1;
            break;
        case 4:
            result.assertions = scene_run_maximum_assertions + 1;
            break;
        case 5:
            report.cases.push_back(result);
            break;
        case 6:
            result.failure =
                SceneRunFailure{SceneRunFailureKind::assertion, SceneRunPhase::tick, 99,
                                Error{ErrorCode::execution_failed, "Wrong tick", {}}};
            result.status = report.status = SceneRunStatus::failed;
            break;
        }
        CHECK_FALSE(encode_scene_run_report(report));
    }
    CHECK_FALSE(encode_scene_run_report(valid, std::string(4097, 'x')));
    auto error = take(
        encode_scene_run_error(Error{ErrorCode::execution_failed, std::string(1025, 'x'), {}}));
    CHECK(error.find("malformed diagnostics") != std::string::npos);
}

TEST_CASE("Scene activation retains its owner while results record fresh incarnations",
          "[scene-runner]") {
    Fixture fixture;
    fixture.config.ticks = 1;
    AuthoredScene* original_owner = nullptr;
    auto test = passing("activate");
    test.setup = [&](SceneRunContext& context) -> Result<void> {
        original_owner = &context.owner();
        auto status = take(context.owner().status());
        auto reply = take(context.owner().execute(CreateEntityCommand{id<AuthoredEntityId>('4')},
                                                  status.revision));
        REQUIRE(reply.outcome);
        return {};
    };
    test.tick = [&](SceneRunContext& context, const SceneRunTick&) -> Result<void> {
        const auto before = take(context.owner().status()).revision;
        auto activated =
            context.owner().activate(fixture.scene, before, UnsavedScenePolicy::discard);
        if (!activated)
            return activated.error();
        return context.require(&context.owner() == original_owner &&
                                   activated.value().scene_instance != before.scene_instance,
                               "Activation changes the scene incarnation within the same owner");
    };
    const std::array cases{test};
    auto report = fixture.run(cases);
    REQUIRE(report.status == SceneRunStatus::passed);
    CHECK(report.cases.front().initial_revision->scene_instance !=
          report.cases.front().final_revision->scene_instance);
    CHECK(encode_scene_run_report(report));
}
