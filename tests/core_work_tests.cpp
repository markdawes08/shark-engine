#include <shark/completion_queue.hpp>
#include <shark/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <latch>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using shark::CompletionPush;

enum class WorkOutcome { completed, failed };

// No callbacks or indirect payload allocation: a worker publishes an owned value
// and only the consumer interprets it. Deleted copy operations exercise the
// move-only contract without smuggling heap ownership or callbacks into the lock.
struct Completion {
    std::uint64_t operation;
    int value;
    WorkOutcome outcome{WorkOutcome::completed};
    shark::ErrorCode error{shark::ErrorCode::execution_failed};

    Completion(const std::uint64_t operation_id, const int result) noexcept
        : operation(operation_id), value(result) {}
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;
    Completion(Completion&&) = default;
    Completion& operator=(Completion&&) = default;
};

struct OversizedCompletion {
    std::array<char, 1024 * 1024> bytes;
};

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
};

static_assert(shark::CompletionQueueValue<Completion, 2>);
static_assert(!std::is_copy_constructible_v<Completion>);
static_assert(!shark::CompletionQueueValue<int, 0>);
static_assert(!shark::CompletionQueueValue<int, 4097>);
static_assert(!shark::CompletionQueueValue<const int, 1>);
static_assert(!shark::CompletionQueueValue<std::string, 1>);
static_assert(!shark::CompletionQueueValue<std::unique_ptr<int>, 1>);
static_assert(!shark::CompletionQueueValue<ThrowingMove, 1>);
static_assert(!shark::CompletionQueueValue<OversizedCompletion, 1>);
static_assert(!std::is_copy_constructible_v<shark::CompletionQueue<int, 1>>);
static_assert(!std::is_move_constructible_v<shark::CompletionQueue<int, 1>>);

// This deadline is a test watchdog, not a scheduling assertion or sleep. Reading
// the count acquires the mutex after the producer releases it into the full-queue
// wait. A latch alone, immediately before wait_push, would not prove that state.
template <typename T, std::size_t Capacity>
bool wait_for_producers(const shark::CompletionQueue<T, Capacity>& queue, const std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (queue.waiting_producers() != count) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Every fixture entry point catches its exceptions. The owner only inspects this
// borrowed output after join, so no test assertion executes on a worker thread.
struct ProducerResult {
    CompletionPush push{CompletionPush::full};
    int retained_value{0};
    std::exception_ptr exception;
};

} // namespace

TEST_CASE("Completion queue validates its bounded inline contract", "[core][work]") {
    shark::CompletionQueue<Completion, 2> queue;
    CHECK(queue.capacity() == 2);
    CHECK(queue.size() == 0);
    CHECK(queue.waiting_producers() == 0);
    CHECK_FALSE(queue.try_pop().has_value());
}

TEST_CASE("Completion queue preserves rejected values and FIFO across ring wraps", "[core][work]") {
    shark::CompletionQueue<Completion, 2> queue;
    for (std::uint64_t cycle = 0; cycle < 128; ++cycle) {
        Completion first{cycle * 3, 11};
        Completion second{cycle * 3 + 1, 22};
        Completion pending{cycle * 3 + 2, 33};
        REQUIRE(queue.try_push(std::move(first)) == CompletionPush::accepted);
        REQUIRE(queue.try_push(std::move(second)) == CompletionPush::accepted);
        REQUIRE(queue.try_push(std::move(pending)) == CompletionPush::full);
        CHECK(pending.operation == cycle * 3 + 2);
        CHECK(pending.value == 33);
        CHECK(queue.size() == queue.capacity());
        auto result = queue.try_pop();
        REQUIRE(result.has_value());
        CHECK(result->operation == cycle * 3);
        CHECK(result->value == 11);
        REQUIRE(queue.try_push(std::move(pending)) == CompletionPush::accepted);
        result = queue.try_pop();
        REQUIRE(result.has_value());
        CHECK(result->operation == cycle * 3 + 1);
        CHECK(result->value == 22);
        result = queue.try_pop();
        REQUIRE(result.has_value());
        CHECK(result->operation == cycle * 3 + 2);
        CHECK(result->value == 33);
        CHECK_FALSE(queue.try_pop().has_value());
    }
}

TEST_CASE("Closing completion admission retains accepted values for owner drain", "[core][work]") {
    shark::CompletionQueue<Completion, 2> queue;
    Completion accepted{1, 71};
    REQUIRE(queue.try_push(std::move(accepted)) == CompletionPush::accepted);
    queue.close();
    queue.close();
    Completion pending{2, 72};
    CHECK(queue.try_push(std::move(pending)) == CompletionPush::closed);
    CHECK(queue.wait_push(std::move(pending), {}) == CompletionPush::closed);
    CHECK(pending.operation == 2);
    CHECK(pending.value == 72);
    const auto result = queue.try_pop();
    REQUIRE(result.has_value());
    CHECK(result->operation == 1);
    CHECK(result->value == 71);
    CHECK_FALSE(queue.try_pop().has_value());
    CHECK(queue.try_push(std::move(pending)) == CompletionPush::closed);
}

TEST_CASE("Observed stop rejects a completion before admission or closure", "[core][work]") {
    shark::CompletionQueue<Completion, 1> queue;
    std::stop_source stop;
    REQUIRE(stop.request_stop());
    Completion pending{91, 19};
    CHECK(queue.wait_push(std::move(pending), stop.get_token()) == CompletionPush::cancelled);
    CHECK(queue.size() == 0);
    CHECK(pending.operation == 91);
    CHECK(pending.value == 19);
    queue.close();
    CHECK(queue.wait_push(std::move(pending), stop.get_token()) == CompletionPush::cancelled);
    CHECK(pending.value == 19);
}

TEST_CASE("Owner consumption wakes a producer blocked by completion backpressure", "[core][work]") {
    shark::CompletionQueue<int, 1> queue;
    REQUIRE(queue.try_push(10) == CompletionPush::accepted);
    ProducerResult result;
    std::latch entered{1};
    std::jthread producer{[&](const std::stop_token stop) noexcept {
        entered.count_down();
        try {
            int pending = 20;
            result.push = queue.wait_push(std::move(pending), stop);
            result.retained_value = pending;
        } catch (...) {
            result.exception = std::current_exception();
        }
    }};
    entered.wait();
    const bool blocked = wait_for_producers(queue, 1);
    if (!blocked) {
        queue.close();
        producer.request_stop();
        producer.join();
    }
    REQUIRE(blocked);
    CHECK(queue.size() == 1);
    CHECK(queue.try_pop() == 10);
    producer.join();
    CHECK_FALSE(result.exception);
    CHECK(result.push == CompletionPush::accepted);
    CHECK(queue.waiting_producers() == 0);
    CHECK(queue.try_pop() == 20);
}

TEST_CASE("Stop wakes a blocked producer without stealing its rejected completion",
          "[core][work]") {
    shark::CompletionQueue<int, 1> queue;
    REQUIRE(queue.try_push(31) == CompletionPush::accepted);
    ProducerResult result;
    std::jthread producer{[&](const std::stop_token stop) noexcept {
        try {
            int pending = 32;
            result.push = queue.wait_push(std::move(pending), stop);
            result.retained_value = pending;
        } catch (...) {
            result.exception = std::current_exception();
        }
    }};
    const bool blocked = wait_for_producers(queue, 1);
    producer.request_stop();
    if (!blocked) {
        queue.close();
    }
    producer.join();
    REQUIRE(blocked);
    CHECK_FALSE(result.exception);
    CHECK(result.push == CompletionPush::cancelled);
    CHECK(result.retained_value == 32);
    CHECK(queue.waiting_producers() == 0);
    CHECK(queue.try_pop() == 31);
    CHECK_FALSE(queue.try_pop().has_value());
}

TEST_CASE("Close wakes all blocked completion producers and preserves prior accepts",
          "[core][work]") {
    shark::CompletionQueue<int, 1> queue;
    REQUIRE(queue.try_push(40) == CompletionPush::accepted);
    std::array<ProducerResult, 3> results{};
    std::array<std::jthread, 3> producers;
    std::barrier start{4};
    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index] = std::jthread{[&, index](const std::stop_token stop) noexcept {
            start.arrive_and_wait();
            try {
                int pending = static_cast<int>(index) + 41;
                results[index].push = queue.wait_push(std::move(pending), stop);
                results[index].retained_value = pending;
            } catch (...) {
                results[index].exception = std::current_exception();
            }
        }};
    }
    start.arrive_and_wait();
    const bool blocked = wait_for_producers(queue, producers.size());
    queue.close();
    for (auto& producer : producers) {
        producer.join();
    }
    REQUIRE(blocked);
    for (std::size_t index = 0; index < results.size(); ++index) {
        CHECK_FALSE(results[index].exception);
        CHECK(results[index].push == CompletionPush::closed);
        CHECK(results[index].retained_value == static_cast<int>(index) + 41);
    }
    CHECK(queue.waiting_producers() == 0);
    CHECK(queue.try_pop() == 40);
    CHECK_FALSE(queue.try_pop().has_value());
}

TEST_CASE("A cancelled waiter leaves space usable by another producer", "[core][work]") {
    shark::CompletionQueue<int, 1> queue;
    REQUIRE(queue.try_push(50) == CompletionPush::accepted);
    std::array<ProducerResult, 2> results{};
    std::array<std::jthread, 2> producers;
    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index] = std::jthread{[&, index](const std::stop_token stop) noexcept {
            try {
                int pending = static_cast<int>(index) + 51;
                results[index].push = queue.wait_push(std::move(pending), stop);
                results[index].retained_value = pending;
            } catch (...) {
                results[index].exception = std::current_exception();
            }
        }};
    }
    const bool blocked = wait_for_producers(queue, producers.size());
    producers[0].request_stop();
    CHECK(queue.try_pop() == 50);
    if (!blocked) {
        queue.close();
        producers[1].request_stop();
    }
    for (auto& producer : producers) {
        producer.join();
    }
    REQUIRE(blocked);
    CHECK_FALSE(results[0].exception);
    CHECK_FALSE(results[1].exception);
    CHECK(results[0].push == CompletionPush::cancelled);
    CHECK(results[0].retained_value == 51);
    CHECK(results[1].push == CompletionPush::accepted);
    CHECK(queue.try_pop() == 52);
    CHECK(queue.waiting_producers() == 0);
}

TEST_CASE("Concurrent completion producers preserve each producer's accepted order",
          "[core][work]") {
    shark::CompletionQueue<Completion, 2> queue;
    std::array<ProducerResult, 2> results{};
    std::array<std::jthread, 2> producers;
    std::barrier start{3};
    for (std::size_t index = 0; index < producers.size(); ++index) {
        producers[index] = std::jthread{[&, index](const std::stop_token stop) noexcept {
            start.arrive_and_wait();
            try {
                for (int sequence = 0; sequence < 32; ++sequence) {
                    Completion pending{index, sequence};
                    results[index].push = queue.wait_push(std::move(pending), stop);
                    if (results[index].push != CompletionPush::accepted) {
                        return;
                    }
                }
            } catch (...) {
                results[index].exception = std::current_exception();
            }
        }};
    }
    start.arrive_and_wait();
    std::array<int, 2> expected{};
    std::size_t consumed = 0;
    bool ordered = true;
    bool bounded = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (consumed < 64 && std::chrono::steady_clock::now() < deadline) {
        bounded = bounded && queue.size() <= queue.capacity();
        if (auto completion = queue.try_pop()) {
            const auto index = static_cast<std::size_t>(completion->operation);
            if (index >= expected.size()) {
                ordered = false;
            } else {
                ordered = ordered && completion->value == expected[index];
                ++expected[index];
            }
            ++consumed;
        } else {
            std::this_thread::yield();
        }
    }
    queue.close();
    for (auto& producer : producers) {
        producer.request_stop();
        producer.join();
    }
    CHECK(consumed == 64);
    CHECK(ordered);
    CHECK(bounded);
    CHECK(expected == std::array<int, 2>{32, 32});
    for (const auto& result : results) {
        CHECK_FALSE(result.exception);
        CHECK(result.push == CompletionPush::accepted);
    }
}

TEST_CASE("Worker exceptions become bounded failure completions at thread entry", "[core][work]") {
    shark::CompletionQueue<Completion, 1> queue;
    ProducerResult result;
    std::jthread producer{[&]() noexcept {
        try {
            Completion completion{123, 0};
            try {
                throw std::runtime_error{"injected worker failure"};
            } catch (...) {
                completion.outcome = WorkOutcome::failed;
                completion.error = shark::ErrorCode::execution_failed;
            }
            result.push = queue.try_push(std::move(completion));
        } catch (...) {
            result.exception = std::current_exception();
        }
    }};
    producer.join();
    CHECK_FALSE(result.exception);
    REQUIRE(result.push == CompletionPush::accepted);
    const auto completion = queue.try_pop();
    REQUIRE(completion.has_value());
    CHECK(completion->operation == 123);
    CHECK(completion->outcome == WorkOutcome::failed);
    CHECK(completion->error == shark::ErrorCode::execution_failed);
}

TEST_CASE("Completion owners repeatedly close stop join and drain before release", "[core][work]") {
    struct LifetimeTrace {
        bool producer_exited{false};
        bool joined{false};
        bool released_after_join{false};
        int committed{0};
        int invalidated{0};
    };
    struct Consumer {
        LifetimeTrace& trace;
        ~Consumer() {
            trace.released_after_join = trace.joined && trace.producer_exited;
        }
    };

    for (int incarnation = 0; incarnation < 32; ++incarnation) {
        LifetimeTrace trace;
        ProducerResult result;
        bool blocked = false;
        {
            shark::CompletionQueue<Completion, 2> queue;
            const Consumer consumer{trace};
            Completion current{static_cast<std::uint64_t>(incarnation), 1};
            Completion stale{static_cast<std::uint64_t>(incarnation + 1), 2};
            REQUIRE(queue.try_push(std::move(current)) == CompletionPush::accepted);
            REQUIRE(queue.try_push(std::move(stale)) == CompletionPush::accepted);
            std::jthread producer{[&](const std::stop_token stop) noexcept {
                try {
                    Completion pending{static_cast<std::uint64_t>(incarnation), 3};
                    result.push = queue.wait_push(std::move(pending), stop);
                    result.retained_value = pending.value;
                } catch (...) {
                    result.exception = std::current_exception();
                }
                trace.producer_exited = true;
            }};
            blocked = wait_for_producers(queue, 1);
            queue.close();
            producer.request_stop();
            producer.join();
            trace.joined = true;
            while (auto completion = queue.try_pop()) {
                if (completion->operation == static_cast<std::uint64_t>(incarnation)) {
                    ++trace.committed;
                } else {
                    ++trace.invalidated;
                }
            }
            CHECK(queue.waiting_producers() == 0);
            CHECK(queue.size() == 0);
        }
        REQUIRE(blocked);
        CHECK_FALSE(result.exception);
        CHECK((result.push == CompletionPush::closed || result.push == CompletionPush::cancelled));
        CHECK(result.retained_value == 3);
        CHECK(trace.producer_exited);
        CHECK(trace.released_after_join);
        CHECK(trace.committed == 1);
        CHECK(trace.invalidated == 1);
    }
}
