#include "durable_worker.hpp"

#include "shark/completion_queue.hpp"
#include "shark/storage/characters.hpp"

#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark::storage::detail {
namespace {
thread_local RuntimeWorkerHook worker_hook = nullptr;
thread_local bool worker_exception = false;
Error worker_error(const char* message, ErrorCode code = ErrorCode::execution_failed) {
    return {code, message, {.operation = "storage.runtime.worker"}};
}
template <typename T> StoreResult boxed(Result<T> result) {
    if (!result)
        return std::move(result).error();
    if constexpr (std::is_void_v<T>)
        return StoreValue{std::monostate{}};
    else
        return StoreValue{std::move(result).value()};
}
} // namespace

void set_runtime_worker_hook(RuntimeWorkerHook hook) noexcept {
    worker_hook = hook;
}
void set_runtime_worker_exception(bool enabled) noexcept {
    worker_exception = enabled;
}

StoreResult execute_store(Store& store, const StoreCommand& command) {
    return std::visit(
        [&](const auto& value) -> StoreResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, ReadMetadata>)
                return boxed(store.metadata());
            else if constexpr (std::is_same_v<T, ReadRegistry>)
                return boxed(store.registry());
            else if constexpr (std::is_same_v<T, ProvisionAccount>)
                return boxed(store.provision_account(value.account));
            else if constexpr (std::is_same_v<T, EnrollCertificate>)
                return boxed(store.enroll_certificate(value.account, value.certificate));
            else if constexpr (std::is_same_v<T, RevokeAccount>)
                return boxed(store.revoke_account(value.account));
            else if constexpr (std::is_same_v<T, RevokeCertificate>)
                return boxed(store.revoke_certificate(value.certificate));
            else if constexpr (std::is_same_v<T, ReadRoster>)
                return boxed(store.roster(value.account));
            else if constexpr (std::is_same_v<T, ReadCharacter>)
                return boxed(store.character(value.account, value.character));
            else if constexpr (std::is_same_v<T, CreateCharacter>)
                return boxed(store.create_character(value.account, value.revision, value.state));
            else if constexpr (std::is_same_v<T, SaveCharacter>)
                return boxed(store.save_character(value.account, value.character, value.revision,
                                                  value.state));
            else if constexpr (std::is_same_v<T, InitializeEconomy>)
                return boxed(store.initialize_economy(value.account, value.character, value.state));
            else if constexpr (std::is_same_v<T, ReadEconomy>)
                return boxed(store.economy(value.account, value.character));
            else if constexpr (std::is_same_v<T, ReadOperationStream>)
                return boxed(store.operation_stream(value.account));
            else
                return boxed(store.transfer(value));
        },
        command);
}

struct RuntimeWorker::Impl {
    struct Slot {
        std::optional<std::uint64_t> sequence;
        std::optional<StoreCommand> command;
        std::optional<StoreResult> result;
        std::optional<StoreResult> failure;
        bool synchronous{false};
    };
    std::filesystem::path path;
    StoreId expected_store;
    WorldId world;
    RuntimeWorkerHook hook;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable ready;
    std::array<Slot, maximum_runtime_pending> slots{};
    CompletionQueue<WorkerToken, maximum_runtime_pending> requests;
    CompletionQueue<WorkerToken, maximum_runtime_pending> completions;
    std::optional<Result<StoreMetadata>> startup;
    Error startup_failure{worker_error("Persistence worker startup threw an exception")};
    bool closing{false};
    bool poisoned{false};
    bool inject_exception{false};
    std::jthread thread;

    Impl(std::filesystem::path value, StoreId id, WorldId expected_world,
         RuntimeWorkerHook callback)
        : path(std::move(value)), expected_store(id), world(expected_world), hook(callback),
          inject_exception(worker_exception) {}
    void phase(RuntimeWorkerPhase value, std::uint64_t sequence = 0) const noexcept {
        if (hook)
            hook(value, sequence);
    }
    void run() noexcept {
        // Store's complete lifetime belongs to this thread, including startup failure.
        std::unique_ptr<Store> store;
        try {
            auto opened = Store::open(path, world);
            Result<StoreMetadata> metadata = opened
                                                 ? opened.value()->metadata()
                                                 : Result<StoreMetadata>{std::move(opened).error()};
            if (metadata && metadata.value().id != expected_store) {
                metadata = worker_error("Reopened database does not match the expected StoreId",
                                        ErrorCode::conflict);
            }
            if (metadata)
                store = std::move(opened).value();
            if (store)
                phase(RuntimeWorkerPhase::opened);
            {
                const std::lock_guard lock{mutex};
                startup.emplace(std::move(metadata));
            }
            ready.notify_all();
            if (!store)
                return;
            for (;;) {
                WorkerToken token{};
                std::optional<StoreCommand> command;
                bool stop = false;
                {
                    std::unique_lock lock{mutex};
                    wake.wait(lock, [&] { return closing || requests.size() != 0; });
                    auto next = requests.try_pop();
                    if (!next)
                        break;
                    token = *next;
                    command = std::move(slots[token.slot].command);
                    slots[token.slot].command.reset();
                    stop = closing || poisoned;
                }
                std::optional<StoreResult> result;
                if (!stop) {
                    phase(RuntimeWorkerPhase::before_execute, token.sequence);
                    try {
                        if (token.sequence != 0 && inject_exception) {
                            inject_exception = false;
                            throw 1;
                        }
                        result.emplace(execute_store(*store, *command));
                    } catch (...) {
                        // Never retry a potentially committed operation after an exception.
                        poisoned = true;
                    }
                    phase(RuntimeWorkerPhase::after_execute, token.sequence);
                }
                if (result && !*result) {
                    const auto code = result->error().code;
                    poisoned = poisoned || code == ErrorCode::io_failure ||
                               code == ErrorCode::execution_failed ||
                               code == ErrorCode::invalid_configuration;
                }
                phase(RuntimeWorkerPhase::before_publish, token.sequence);
                bool synchronous = false;
                {
                    const std::lock_guard lock{mutex};
                    auto& slot = slots[token.slot];
                    slot.result = result ? std::move(result) : std::move(slot.failure);
                    slot.failure.reset();
                    synchronous = slot.synchronous;
                    if (!synchronous) {
                        // At most one token per retained slot; the fixed ring cannot fill
                        // before every slot already owns its one completion.
                        if (completions.try_push(std::move(token)) != CompletionPush::accepted) {
                            std::terminate();
                        }
                    }
                }
                ready.notify_all();
            }
            phase(RuntimeWorkerPhase::closing);
            store.reset();
            phase(RuntimeWorkerPhase::closed);
        } catch (...) {
            // Standard synchronization/startup failures must not escape the worker.
            // Preallocated per-slot failures avoid allocating diagnostics here.
            try {
                const std::lock_guard lock{mutex};
                closing = true;
                if (!startup)
                    startup.emplace(std::move(startup_failure));
                for (std::size_t index = 0; index < slots.size(); ++index) {
                    auto& slot = slots[index];
                    if (slot.sequence && !slot.result) {
                        slot.result = std::move(slot.failure);
                        slot.command.reset();
                        if (!slot.synchronous) {
                            WorkerToken token{index, *slot.sequence};
                            if (completions.try_push(std::move(token)) != CompletionPush::accepted)
                                std::terminate();
                        }
                    }
                }
                ready.notify_all();
            } catch (...) {
                std::terminate();
            }
        }
    }
    ~Impl() {
        if (thread.joinable()) {
            {
                const std::lock_guard lock{mutex};
                closing = true;
                requests.close();
            }
            phase(RuntimeWorkerPhase::shutdown_requested);
            wake.notify_all();
            thread.request_stop();
            thread.join();
        }
        completions.close();
        while (completions.try_pop()) {
        }
    }
};

RuntimeWorker::RuntimeWorker(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RuntimeWorker::~RuntimeWorker() = default;

Result<std::unique_ptr<RuntimeWorker>> RuntimeWorker::open(const std::filesystem::path& path,
                                                           StoreId expected_store, WorldId world) {
    auto impl = std::make_unique<Impl>(path, expected_store, world, worker_hook);
    impl->thread = std::jthread([owner = impl.get()] { owner->run(); });
    {
        std::unique_lock lock{impl->mutex};
        impl->ready.wait(lock, [&] { return impl->startup.has_value(); });
        if (!*impl->startup)
            return std::move(*impl->startup).error();
    }
    return std::unique_ptr<RuntimeWorker>{new RuntimeWorker{std::move(impl)}};
}

Result<void> RuntimeWorker::submit(WorkerToken token, StoreCommand command) {
    // Allocate the exception result before admitting work to the worker.
    StoreResult fallback{
        worker_error("Persistence work requires explicit reopen and reconciliation")};
    const std::lock_guard lock{impl_->mutex};
    if (token.slot >= impl_->slots.size() || impl_->closing || impl_->slots[token.slot].sequence) {
        return worker_error("Persistence worker slot is unavailable", ErrorCode::conflict);
    }
    auto& slot = impl_->slots[token.slot];
    slot.sequence = token.sequence;
    slot.synchronous = token.sequence == 0;
    slot.failure.emplace(std::move(fallback));
    slot.command.emplace(std::move(command));
    if (impl_->requests.try_push(std::move(token)) != CompletionPush::accepted) {
        slot = {};
        return worker_error("Persistence worker admission is closed", ErrorCode::conflict);
    }
    impl_->wake.notify_one();
    return {};
}

StoreResult RuntimeWorker::execute(StoreCommand command) {
    constexpr WorkerToken token{0, 0};
    if (auto admitted = submit(token, std::move(command)); !admitted)
        return admitted.error();
    {
        std::unique_lock lock{impl_->mutex};
        impl_->ready.wait(lock, [&] { return impl_->slots[0].result.has_value(); });
    }
    return take(token);
}

std::optional<WorkerToken> RuntimeWorker::poll() {
    return impl_->completions.try_pop();
}

StoreResult RuntimeWorker::take(WorkerToken token) {
    const std::lock_guard lock{impl_->mutex};
    if (token.slot >= impl_->slots.size())
        return worker_error("Unknown persistence worker slot");
    auto& slot = impl_->slots[token.slot];
    if (!slot.sequence || *slot.sequence != token.sequence || !slot.result)
        return worker_error("Persistence worker completion does not match its request");
    auto result = std::move(*slot.result);
    slot = {};
    return result;
}

} // namespace shark::storage::detail
