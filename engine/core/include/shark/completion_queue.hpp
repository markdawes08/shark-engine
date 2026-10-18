#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace shark {

// Bound the inline storage, not memory reachable through a user-supplied pointer.
// Payloads must be owned, bounded value envelopes, without borrowed mutable state.
// The constraints prevent user move/destruction callbacks inside the queue lock.
template <typename T, std::size_t Capacity>
concept CompletionQueueValue =
    std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T> &&
    std::is_trivially_move_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
    std::is_trivially_destructible_v<T> && (Capacity > 0 && Capacity <= 4096) &&
    (sizeof(std::optional<T>) <= 1024 * 1024 / Capacity);

enum class CompletionPush { accepted, full, closed, cancelled };

// A bounded multi-producer/single-owner completion channel, not a job executor.
// Successful pushes transfer a value; every rejection leaves its argument intact.
// FIFO means accepted lock order, not deterministic ordering between producers.
// Only the owner consumes and applies results; producers never mutate owner state.
//
// close() stops admission and wakes waiters but retains accepted values for drain.
// The owner closes admission, requests producer stop, joins every producer, then
// drains or invalidates accepted values before destroying the queue/consumer.
// No borrowed thread is joined here, and cancellation cannot interrupt arbitrary
// producer computation. The owner must bound that work and catch its exceptions.
//
// These locking APIs are not eligible for audio, signal or real-time callbacks.
// No payload allocation or callback occurs during transfer. Construction and
// standard-library synchronization can throw; worker entry points contain them.
template <typename T, std::size_t Capacity>
    requires CompletionQueueValue<T, Capacity>
class CompletionQueue final {
  public:
    CompletionQueue() = default;
    ~CompletionQueue() = default;
    CompletionQueue(const CompletionQueue&) = delete;
    CompletionQueue& operator=(const CompletionQueue&) = delete;
    CompletionQueue(CompletionQueue&&) = delete;
    CompletionQueue& operator=(CompletionQueue&&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    [[nodiscard]] CompletionPush try_push(T&& value) {
        const std::lock_guard lock{mutex_};
        if (closed_) {
            return CompletionPush::closed;
        }
        if (size_ == Capacity) {
            return CompletionPush::full;
        }
        push_locked(std::move(value));
        return CompletionPush::accepted;
    }

    // Observed cancellation takes precedence over closure and available capacity.
    // Cancellation racing acceptance may lose after the final token observation;
    // an accepted value remains accepted and must be drained or invalidated.
    [[nodiscard]] CompletionPush wait_push(T&& value, const std::stop_token stop) {
        std::unique_lock lock{mutex_};
        if (!stop.stop_requested() && !closed_ && size_ == Capacity) {
            ++waiting_producers_;
            try {
                static_cast<void>(space_available_.wait(
                    lock, stop, [this] { return closed_ || size_ < Capacity; }));
            } catch (...) {
                --waiting_producers_;
                throw;
            }
            --waiting_producers_;
        }
        if (stop.stop_requested()) {
            // If this waiter consumed a space notification, pass it on rather
            // than leaving another producer asleep while capacity is available.
            const bool has_space = !closed_ && size_ < Capacity;
            lock.unlock();
            if (has_space) {
                space_available_.notify_one();
            }
            return CompletionPush::cancelled;
        }
        if (closed_) {
            return CompletionPush::closed;
        }
        push_locked(std::move(value));
        return CompletionPush::accepted;
    }

    // An empty result means there is no accepted value at this instant. The owner
    // determines end-of-stream by closing and joining producers before final drain.
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock lock{mutex_};
        if (size_ == 0) {
            return std::nullopt;
        }
        std::optional<T> value{std::in_place, std::move(*slots_[head_])};
        slots_[head_].reset();
        head_ = (head_ + 1) % Capacity;
        --size_;
        lock.unlock();
        space_available_.notify_one();
        return value;
    }

    void close() {
        {
            const std::lock_guard lock{mutex_};
            closed_ = true;
        }
        space_available_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock{mutex_};
        return size_;
    }

    // Diagnostic snapshot only; it is not an admission or scheduling guarantee.
    [[nodiscard]] std::size_t waiting_producers() const {
        const std::lock_guard lock{mutex_};
        return waiting_producers_;
    }

  private:
    void push_locked(T&& value) noexcept {
        const auto tail = (head_ + size_) % Capacity;
        slots_[tail].emplace(std::move(value));
        ++size_;
    }

    std::array<std::optional<T>, Capacity> slots_{};
    mutable std::mutex mutex_;
    std::condition_variable_any space_available_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::size_t waiting_producers_{0};
    bool closed_{false};
};

} // namespace shark
