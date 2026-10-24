#include "shark/storage/characters.hpp"
#include "durable_worker.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark::storage {
namespace {
Error rejected(const char* operation, const char* message = "Character operation rejected",
               ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}

Result<std::unique_ptr<net::SessionAuthority>> hydrate(const DurableRegistry& records,
                                                       const net::SessionLimits& limits) {
    if (records.accounts.size() > limits.accounts ||
        records.characters.size() > limits.characters ||
        records.certificates.size() > limits.certificates) {
        return rejected("storage.characters.create_authority",
                        "Durable registry exceeds session admission capacity",
                        ErrorCode::invalid_configuration);
    }
    auto sessions = net::SessionAuthority::create(limits);
    if (!sessions)
        return sessions.error();
    for (const auto& account : records.accounts)
        if (auto result = sessions.value()->provision_account(account.account); !result)
            return result.error();
    for (const auto& character : records.characters)
        if (auto result =
                sessions.value()->assign_character(character.account, character.character);
            !result)
            return result.error();
    for (const auto& enrollment : records.certificates)
        if (auto result =
                sessions.value()->enroll_certificate(enrollment.account, enrollment.certificate);
            !result)
            return result.error();
    for (const auto& enrollment : records.certificates)
        if (enrollment.revoked)
            if (auto result = sessions.value()->revoke_certificate(enrollment.certificate); !result)
                return result.error();
    for (const auto& account : records.accounts)
        if (account.revoked)
            if (auto result = sessions.value()->revoke_account(account.account); !result)
                return result.error();
    return sessions;
}

bool needs_reconciliation(const Error& error) {
    return error.code == ErrorCode::io_failure || error.code == ErrorCode::execution_failed ||
           error.code == ErrorCode::invalid_configuration;
}
} // namespace

struct CharacterAuthority::Impl {
    std::unique_ptr<Store> store;
    std::unique_ptr<net::SessionAuthority> sessions;
    net::SessionLimits limits;
    WorldId world;
    std::thread::id thread;
    bool faulted{false};
    struct Binding {
        net::AccountId account;
        net::CharacterId character;
        WorldEntityId entity;
    };
    struct Pending {
        RuntimeTicket ticket;
        net::SessionSnapshot session;
        EntityReference entity;
        MonotonicTime deadline;
        std::optional<TransferRequest> transfer;
        std::optional<Result<RuntimeValue>> outcome;
        bool timed_out{false};
    };
    std::unique_ptr<detail::RuntimeWorker> worker;
    std::filesystem::path runtime_path;
    std::optional<StoreId> expected_store;
    std::optional<RuntimeId> runtime_id;
    RuntimeLimits runtime_limits;
    std::vector<Binding> bindings;
    std::array<std::optional<Pending>, maximum_runtime_pending> pending{};
    std::uint64_t last_ticket{0};
    MonotonicTime last_time{0};

    Impl(std::unique_ptr<Store> owned_store, std::unique_ptr<net::SessionAuthority> owned_sessions,
         net::SessionLimits session_limits, WorldId store_world)
        : store(std::move(owned_store)), sessions(std::move(owned_sessions)),
          limits(session_limits), world(store_world), thread(std::this_thread::get_id()) {
        bindings.reserve(maximum_durable_characters);
    }

    ~Impl() {
        // Join before releasing retained owner outcomes and admission context.
        worker.reset();
    }

    std::size_t retained() const {
        return static_cast<std::size_t>(std::count_if(
            pending.begin(), pending.end(), [](const auto& slot) { return slot.has_value(); }));
    }

    Result<void> owner(const char* operation) const {
        if (std::this_thread::get_id() != thread)
            return rejected(operation, "Character authority requires its creating thread");
        return {};
    }

    Result<void> check(const char* operation, bool maintenance = false) const {
        if (auto checked = owner(operation); !checked)
            return checked;
        if (faulted) {
            return rejected(operation, "Character authority requires reopen and reconciliation",
                            ErrorCode::execution_failed);
        }
        if (maintenance && runtime_id && retained() != 0)
            return rejected(operation,
                            "Blocking storage maintenance requires no retained runtime work");
        return {};
    }

    template <typename T> Result<T> call(detail::StoreCommand command) const {
        if (auto checked = check("storage.characters.maintenance", true); !checked)
            return checked.error();
        auto result =
            worker ? worker->execute(std::move(command)) : detail::execute_store(*store, command);
        if (!result)
            return std::move(result).error();
        if constexpr (std::is_void_v<T>) {
            if (!std::holds_alternative<std::monostate>(result.value()))
                return rejected("storage.characters.maintenance", "Unexpected storage result",
                                ErrorCode::execution_failed);
            return {};
        } else {
            auto* value = std::get_if<T>(&result.value());
            if (!value)
                return rejected("storage.characters.maintenance", "Unexpected storage result",
                                ErrorCode::execution_failed);
            return std::move(*value);
        }
    }

    Error storage_error(Error error) {
        if (needs_reconciliation(error)) {
            faulted = true;
        }
        return error;
    }

    // Publication follows durable commit and is preflighted against the sole
    // session owner's limits. An unexpected cache failure cannot leave admission
    // open with durable state different from its authorization view.
    Result<void> publish(Result<void> result) {
        if (!result) {
            faulted = true;
            return rejected("storage.characters.publish",
                            "Durable change committed; reopen authority to reconcile",
                            ErrorCode::execution_failed);
        }
        return {};
    }

    Result<net::SessionSnapshot> current(net::TlsTransport& transport,
                                         const net::SessionEnvelope& envelope, MonotonicTime now,
                                         const char* operation, bool maintenance = true) {
        if (auto checked = check(operation, maintenance); !checked) {
            return checked.error();
        }
        auto result = sessions->current(transport, now);
        if (!result) {
            return result.error();
        }
        const auto& session = result.value();
        if (!session.ready || !session.active || session.session != envelope.session ||
            session.character != envelope.character ||
            session.ownership_epoch != envelope.ownership_epoch ||
            session.last_sequence == std::numeric_limits<std::uint64_t>::max() ||
            envelope.sequence != session.last_sequence + 1) {
            return rejected(operation);
        }
        return result;
    }

    Result<void> runtime_time(MonotonicTime now) {
        if (now.count() < 0 || now < last_time)
            return rejected("storage.runtime.time", "Runtime observations must not regress",
                            ErrorCode::invalid_argument);
        last_time = now;
        return {};
    }

    Result<void> submission_preflight(MonotonicTime deadline, MonotonicTime now) {
        constexpr auto operation = "storage.runtime.submit";
        if (!runtime_id || !worker)
            return rejected(operation, "Runtime submission requires open_runtime",
                            ErrorCode::invalid_configuration);
        if (now.count() < 0 || now < last_time || deadline <= now ||
            deadline - now > std::chrono::seconds{60})
            return rejected(operation, "Runtime deadline must be within sixty monotonic seconds",
                            ErrorCode::invalid_argument);
        for (auto& slot : pending)
            if (slot && now >= slot->deadline) {
                slot->timed_out = true;
                faulted = true;
            }
        if (faulted) {
            last_time = now;
            return rejected(operation, "A retained operation requires reconciliation",
                            ErrorCode::execution_failed);
        }
        if (retained() >= runtime_limits.pending ||
            last_ticket == std::numeric_limits<std::uint64_t>::max())
            return rejected(operation, "Runtime request capacity or ticket sequence is exhausted");
        return {};
    }

    RuntimeStats stats() const {
        RuntimeStats result{runtime_limits.pending, 0, 0, 0, 0, faulted};
        for (const auto& slot : pending)
            if (slot) {
                ++result.retained;
                if (slot->outcome)
                    ++result.completed;
                else
                    ++result.unsettled;
                if (slot->timed_out)
                    ++result.timed_out;
            }
        return result;
    }

    Result<EntityReference> binding(Scene& scene, EntityHandle handle,
                                    const net::SessionSnapshot& session) const {
        auto reference = scene.reference(handle);
        if (!reference)
            return reference.error();
        const auto* scope = std::get_if<RegionSceneScope>(&reference.value().scope);
        const auto* identity = std::get_if<WorldEntityId>(&reference.value().identity);
        const auto found =
            std::find_if(bindings.begin(), bindings.end(), [&](const Binding& value) {
                return value.account == session.account && value.character == session.character;
            });
        if (!scope || scope->world != world || !identity || found == bindings.end() ||
            found->entity != *identity)
            return rejected("storage.runtime.scene",
                            "Runtime intent requires its owned authoritative world entity");
        return reference;
    }

    Result<RuntimeTicket> submit(net::TlsTransport& transport, const net::SessionEnvelope& envelope,
                                 Scene& scene, EntityHandle entity, detail::StoreCommand command,
                                 std::optional<TransferRequest> transfer, MonotonicTime deadline,
                                 MonotonicTime now);
};

CharacterAuthority::CharacterAuthority(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CharacterAuthority::~CharacterAuthority() = default;

Result<std::unique_ptr<CharacterAuthority>>
CharacterAuthority::create(std::unique_ptr<Store> store, const net::SessionLimits& limits) {
    constexpr auto operation = "storage.characters.create_authority";
    if (!store) {
        return rejected(operation, "A durable store owner is required",
                        ErrorCode::invalid_argument);
    }
    auto metadata = store->metadata();
    if (!metadata) {
        return metadata.error();
    }
    auto registry = store->registry();
    if (!registry) {
        return registry.error();
    }
    auto sessions = hydrate(registry.value(), limits);
    if (!sessions)
        return sessions.error();
    auto impl = std::make_unique<Impl>(std::move(store), std::move(sessions).value(), limits,
                                       metadata.value().world);
    for (const auto& character : registry.value().characters)
        impl->bindings.push_back({character.account, character.character, character.entity});
    return std::unique_ptr<CharacterAuthority>(new CharacterAuthority(std::move(impl)));
}

Result<std::unique_ptr<CharacterAuthority>>
CharacterAuthority::open_runtime(const std::filesystem::path& path, StoreId expected_store,
                                 WorldId expected_world, const net::SessionLimits& limits,
                                 RuntimeLimits runtime_limits) {
    if (runtime_limits.pending == 0 || runtime_limits.pending > maximum_runtime_pending)
        return rejected("storage.runtime.open", "Runtime capacity must be from one through sixteen",
                        ErrorCode::invalid_argument);
    auto identity = generate_stable_id();
    if (!identity)
        return identity.error();
    auto worker = detail::RuntimeWorker::open(path, expected_store, expected_world);
    if (!worker)
        return worker.error();
    auto registry = worker.value()->execute(detail::ReadRegistry{});
    if (!registry)
        return registry.error();
    const auto& records = std::get<DurableRegistry>(registry.value());
    auto sessions = hydrate(records, limits);
    if (!sessions)
        return sessions.error();
    auto impl =
        std::make_unique<Impl>(nullptr, std::move(sessions).value(), limits, expected_world);
    impl->worker = std::move(worker).value();
    impl->runtime_path = path;
    impl->expected_store = expected_store;
    impl->runtime_id = RuntimeId{identity.value()};
    impl->runtime_limits = runtime_limits;
    for (const auto& character : records.characters)
        impl->bindings.push_back({character.account, character.character, character.entity});
    return std::unique_ptr<CharacterAuthority>(new CharacterAuthority(std::move(impl)));
}

Result<StoreMetadata> CharacterAuthority::metadata() const {
    if (auto result = impl_->check("storage.characters.metadata", true); !result) {
        return result.error();
    }
    return impl_->call<StoreMetadata>(detail::ReadMetadata{});
}

Result<void> CharacterAuthority::provision_account(net::AccountId account) {
    constexpr auto operation = "storage.characters.provision";
    if (auto result = impl_->check(operation, true); !result) {
        return result;
    }
    auto stats = impl_->sessions->stats();
    if (!stats) {
        return stats.error();
    }
    if (stats.value().accounts >= impl_->limits.accounts) {
        return rejected(operation, "Account admission capacity exhausted");
    }
    auto stored = impl_->call<AccountRecord>(detail::ProvisionAccount{account});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->provision_account(account));
}

Result<void>
CharacterAuthority::enroll_certificate(net::AccountId account,
                                       const net::CertificateFingerprint& certificate) {
    constexpr auto operation = "storage.characters.enroll";
    if (auto result = impl_->check(operation, true); !result) {
        return result;
    }
    auto stats = impl_->sessions->stats();
    if (!stats) {
        return stats.error();
    }
    if (stats.value().certificates >= impl_->limits.certificates) {
        return rejected(operation, "Certificate admission capacity exhausted");
    }
    auto stored = impl_->call<void>(detail::EnrollCertificate{account, certificate});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->enroll_certificate(account, certificate));
}

Result<void> CharacterAuthority::revoke_account(net::AccountId account) {
    if (auto result = impl_->check("storage.characters.revoke_account", true); !result) {
        return result;
    }
    auto stored = impl_->call<void>(detail::RevokeAccount{account});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->revoke_account(account));
}

Result<void>
CharacterAuthority::revoke_certificate(const net::CertificateFingerprint& certificate) {
    if (auto result = impl_->check("storage.characters.revoke_certificate", true); !result) {
        return result;
    }
    auto stored = impl_->call<void>(detail::RevokeCertificate{certificate});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->revoke_certificate(certificate));
}

Result<bool> CharacterAuthority::revoke_session(net::SessionId session) {
    if (auto result = impl_->check("storage.characters.revoke_session"); !result) {
        return result.error();
    }
    return impl_->sessions->revoke_session(session);
}

Result<EconomyRecord> CharacterAuthority::initialize_economy(net::AccountId account,
                                                             net::CharacterId character,
                                                             const EconomyState& initial) {
    if (auto result = impl_->check("storage.characters.initialize_economy", true); !result) {
        return result.error();
    }
    auto stored =
        impl_->call<EconomyRecord>(detail::InitializeEconomy{account, character, initial});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<CharacterRoster> CharacterAuthority::roster(net::TlsTransport& transport,
                                                   net::AccountId account, MonotonicTime now) {
    if (auto result = impl_->check("storage.characters.roster", true); !result) {
        return result.error();
    }
    auto admitted = impl_->sessions->authorize_account(transport, account, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored = impl_->call<CharacterRoster>(detail::ReadRoster{admitted.value()});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<CharacterRecord> CharacterAuthority::create_character(net::TlsTransport& transport,
                                                             net::AccountId account,
                                                             std::uint64_t expected_roster_revision,
                                                             const CharacterState& initial,
                                                             MonotonicTime now) {
    constexpr auto operation = "storage.characters.create";
    if (auto result = impl_->check(operation, true); !result) {
        return result.error();
    }
    auto admitted = impl_->sessions->authorize_account(transport, account, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stats = impl_->sessions->stats();
    if (!stats) {
        return stats.error();
    }
    if (stats.value().characters >= impl_->limits.characters) {
        return rejected(operation, "Character admission capacity exhausted");
    }
    auto stored = impl_->call<CharacterRecord>(
        detail::CreateCharacter{admitted.value(), expected_roster_revision, initial});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    if (auto result = impl_->publish(
            impl_->sessions->assign_character(admitted.value(), stored.value().character));
        !result) {
        return result.error();
    }
    impl_->bindings.push_back(
        {stored.value().account, stored.value().character, stored.value().entity});
    return stored;
}

Result<net::SessionSnapshot> CharacterAuthority::apply(net::TlsTransport& transport,
                                                       const net::SessionRequest& request,
                                                       MonotonicTime now) {
    if (auto result = impl_->check("storage.characters.session"); !result) {
        return result.error();
    }
    return impl_->sessions->apply(transport, request, now);
}

Result<CharacterRecord> CharacterAuthority::load(net::TlsTransport& transport,
                                                 const net::SessionEnvelope& authority,
                                                 MonotonicTime now) {
    auto session = impl_->current(transport, authority, now, "storage.characters.load");
    if (!session) {
        return session.error();
    }
    auto admitted = impl_->sessions->apply(transport, net::HeartbeatRequest{authority}, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored = impl_->call<CharacterRecord>(
        detail::ReadCharacter{session.value().account, session.value().character});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<CharacterRecord> CharacterAuthority::save(net::TlsTransport& transport,
                                                 const net::SessionEnvelope& authority,
                                                 std::uint64_t expected_revision,
                                                 const CharacterState& accepted_state,
                                                 MonotonicTime now) {
    constexpr auto operation = "storage.characters.save";
    auto session = impl_->current(transport, authority, now, operation);
    if (!session) {
        return session.error();
    }
    if (auto result = validate_character_state(accepted_state, impl_->world); !result) {
        return result.error();
    }
    auto admitted = impl_->sessions->apply(transport, net::HeartbeatRequest{authority}, now);
    if (!admitted) {
        return admitted.error();
    }
    auto saved = impl_->call<CharacterRecord>(detail::SaveCharacter{
        session.value().account, session.value().character, expected_revision, accepted_state});
    if (!saved) {
        return impl_->storage_error(saved.error());
    }
    return saved;
}

Result<EconomyRecord> CharacterAuthority::economy(net::TlsTransport& transport,
                                                  const net::SessionEnvelope& authority,
                                                  MonotonicTime now) {
    auto session = impl_->current(transport, authority, now, "storage.characters.economy");
    if (!session) {
        return session.error();
    }
    auto admitted = impl_->sessions->apply(transport, net::HeartbeatRequest{authority}, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored = impl_->call<EconomyRecord>(
        detail::ReadEconomy{session.value().account, session.value().character});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<OperationStream> CharacterAuthority::operation_stream(net::TlsTransport& transport,
                                                             const net::SessionEnvelope& authority,
                                                             MonotonicTime now) {
    auto session = impl_->current(transport, authority, now, "storage.characters.operation_stream");
    if (!session) {
        return session.error();
    }
    auto admitted = impl_->sessions->apply(transport, net::HeartbeatRequest{authority}, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored =
        impl_->call<OperationStream>(detail::ReadOperationStream{session.value().account});
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<TransferResult> CharacterAuthority::transfer(net::TlsTransport& transport,
                                                    const net::SessionEnvelope& authority,
                                                    const TransferRequest& request,
                                                    MonotonicTime now) {
    constexpr auto operation = "storage.characters.transfer";
    auto session = impl_->current(transport, authority, now, operation);
    if (!session) {
        return session.error();
    }
    // Durable retry identity is separate from the live command envelope. A new
    // session may resolve an owned old operation; copied account/character IDs
    // never authorize it, even if its retained receipt exists.
    if (request.operation.actor != session.value().account ||
        request.source != session.value().character) {
        return rejected(operation);
    }
    if (auto result = validate_transfer_request(request); !result) {
        return result.error();
    }
    auto admitted = impl_->sessions->apply(transport, net::HeartbeatRequest{authority}, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored = impl_->call<TransferResult>(request);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<RuntimeTicket> CharacterAuthority::Impl::submit(net::TlsTransport& transport,
                                                       const net::SessionEnvelope& envelope,
                                                       Scene& scene, EntityHandle entity,
                                                       detail::StoreCommand command,
                                                       std::optional<TransferRequest> transfer,
                                                       MonotonicTime deadline, MonotonicTime now) {
    constexpr auto operation = "storage.runtime.submit";
    if (auto checked = check(operation); !checked)
        return checked.error();
    if (auto checked = submission_preflight(deadline, now); !checked)
        return checked.error();
    auto session = current(transport, envelope, now, operation, false);
    if (!session)
        return session.error();
    if (std::any_of(pending.begin(), pending.end(), [&](const auto& slot) {
            return slot && slot->session.account == session.value().account;
        }))
        return rejected(operation, "Account already has a retained runtime operation");
    if (transfer && (transfer->operation.actor != session.value().account ||
                     transfer->source != session.value().character))
        return rejected(operation);
    auto reference = binding(scene, entity, session.value());
    if (!reference)
        return reference.error();
    // Save's account/character are derived from live admission, never caller IDs.
    if (auto* save = std::get_if<detail::SaveCharacter>(&command)) {
        save->account = session.value().account;
        save->character = session.value().character;
    }
    const auto position =
        std::find_if(pending.begin(), pending.end(), [](const auto& slot) { return !slot; });
    const auto index = static_cast<std::size_t>(position - pending.begin());
    const RuntimeTicket ticket{*runtime_id, last_ticket + 1};
    Pending candidate{ticket,   session.value(), std::move(reference).value(),
                      deadline, transfer,        std::nullopt,
                      false};
    auto admitted = sessions->apply(transport, net::HeartbeatRequest{envelope}, now);
    if (!admitted)
        return admitted.error();
    last_time = now;
    last_ticket = ticket.sequence;
    position->emplace(std::move(candidate));
    auto accepted = worker->submit({index, ticket.sequence}, std::move(command));
    if (!accepted) {
        position->reset();
        faulted = true;
        return accepted.error();
    }
    return ticket;
}

Result<RuntimeTicket> CharacterAuthority::submit_save(net::TlsTransport& transport,
                                                      const net::SessionEnvelope& authority,
                                                      Scene& scene, EntityHandle entity,
                                                      std::uint64_t expected_revision,
                                                      const CharacterState& accepted_state,
                                                      MonotonicTime deadline, MonotonicTime now) {
    if (auto checked = impl_->check("storage.runtime.save"); !checked)
        return checked.error();
    if (auto validated = validate_character_state(accepted_state, impl_->world); !validated)
        return validated.error();
    if (expected_revision == 0 || expected_revision > maximum_durable_revision)
        return rejected("storage.runtime.save", "Invalid expected character revision",
                        ErrorCode::invalid_argument);
    if (auto checked = impl_->submission_preflight(deadline, now); !checked)
        return checked.error();
    // Fill the IDs from the validated current session; the temporary values are
    // replaced inside submit before the command can reach the worker.
    auto session = impl_->current(transport, authority, now, "storage.runtime.save", false);
    if (!session)
        return session.error();
    return impl_->submit(transport, authority, scene, entity,
                         detail::SaveCharacter{session.value().account, session.value().character,
                                               expected_revision, accepted_state},
                         std::nullopt, deadline, now);
}

Result<RuntimeTicket> CharacterAuthority::submit_transfer(net::TlsTransport& transport,
                                                          const net::SessionEnvelope& authority,
                                                          Scene& scene, EntityHandle entity,
                                                          const TransferRequest& request,
                                                          MonotonicTime deadline,
                                                          MonotonicTime now) {
    if (auto checked = impl_->check("storage.runtime.transfer"); !checked)
        return checked.error();
    if (auto validated = validate_transfer_request(request); !validated)
        return validated.error();
    return impl_->submit(transport, authority, scene, entity, request, request, deadline, now);
}

Result<RuntimeStats> CharacterAuthority::poll(MonotonicTime now) {
    constexpr auto operation = "storage.runtime.poll";
    if (auto checked = impl_->owner(operation); !checked)
        return checked.error();
    if (!impl_->runtime_id)
        return rejected(operation, "Runtime polling requires open_runtime",
                        ErrorCode::invalid_configuration);
    if (auto checked = impl_->runtime_time(now); !checked)
        return checked.error();
    if (impl_->worker) {
        while (auto token = impl_->worker->poll()) {
            if (token->slot >= impl_->pending.size() || !impl_->pending[token->slot] ||
                impl_->pending[token->slot]->ticket.sequence != token->sequence ||
                impl_->pending[token->slot]->ticket.owner != *impl_->runtime_id) {
                impl_->faulted = true;
                return rejected(operation,
                                "Completion identity does not match its retained operation",
                                ErrorCode::execution_failed);
            }
            auto& slot = *impl_->pending[token->slot];
            auto result = impl_->worker->take(*token);
            if (!result) {
                slot.outcome.emplace(impl_->storage_error(std::move(result).error()));
            } else if (auto* saved = std::get_if<CharacterRecord>(&result.value());
                       saved && !slot.transfer) {
                slot.outcome.emplace(RuntimeValue{std::move(*saved)});
            } else if (auto* transferred = std::get_if<TransferResult>(&result.value());
                       transferred && slot.transfer) {
                slot.outcome.emplace(RuntimeValue{std::move(*transferred)});
            } else {
                impl_->faulted = true;
                slot.outcome.emplace(rejected(operation,
                                              "Completion has an unexpected operation kind",
                                              ErrorCode::execution_failed));
            }
        }
    }
    for (auto& slot : impl_->pending)
        if (slot && now >= slot->deadline) {
            slot->timed_out = true;
            impl_->faulted = true;
        }
    return impl_->stats();
}

Result<RuntimeStats> CharacterAuthority::runtime_stats() const {
    if (auto checked = impl_->owner("storage.runtime.stats"); !checked)
        return checked.error();
    if (!impl_->runtime_id)
        return rejected("storage.runtime.stats", "Runtime statistics require open_runtime",
                        ErrorCode::invalid_configuration);
    return impl_->stats();
}

Result<RuntimeCompletion> CharacterAuthority::take(RuntimeTicket ticket,
                                                   net::TlsTransport& transport, Scene& scene,
                                                   MonotonicTime now) {
    constexpr auto operation = "storage.runtime.take";
    if (auto checked = impl_->owner(operation); !checked)
        return checked.error();
    if (!impl_->runtime_id || ticket.owner != *impl_->runtime_id || ticket.sequence == 0)
        return rejected(operation, "Unknown runtime owner or ticket");
    const auto position =
        std::find_if(impl_->pending.begin(), impl_->pending.end(),
                     [&](const auto& slot) { return slot && slot->ticket == ticket; });
    if (position == impl_->pending.end())
        return rejected(operation, "Runtime ticket is not retained");
    if (auto result = poll(now); !result)
        return result.error();
    auto& slot = **position;
    RuntimeCompletion completion{
        ticket, slot.timed_out ? RuntimeDelivery::timed_out : RuntimeDelivery::pending,
        slot.transfer, std::nullopt};
    if (!slot.outcome)
        return completion;
    auto live = impl_->sessions->current(transport, now);
    const bool authorized = live && live.value().active && live.value().ready &&
                            live.value().session == slot.session.session &&
                            live.value().account == slot.session.account &&
                            live.value().character == slot.session.character &&
                            live.value().ownership_epoch == slot.session.ownership_epoch;
    auto resolved = scene.resolve(slot.entity);
    if (!slot.timed_out) {
        if (!authorized || !resolved)
            completion.delivery = RuntimeDelivery::fenced;
        else {
            completion.delivery = !*slot.outcome && needs_reconciliation(slot.outcome->error())
                                      ? RuntimeDelivery::reconciliation_required
                                      : RuntimeDelivery::ready;
        }
    }
    // Settlement is a trusted owner result even when its player/scene delivery
    // is fenced or late. Retain it for explicit reconciliation, never publish it
    // to gameplay merely because a durable operation completed.
    completion.outcome = std::move(slot.outcome);
    position->reset();
    return completion;
}

Result<void> CharacterAuthority::recover() {
    constexpr auto operation = "storage.runtime.recover";
    if (auto checked = impl_->owner(operation); !checked)
        return checked;
    if (!impl_->runtime_id || !impl_->expected_store)
        return rejected(operation, "Runtime recovery requires open_runtime",
                        ErrorCode::invalid_configuration);
    if (impl_->retained() != 0)
        return rejected(operation,
                        "Settle and take every retained runtime outcome before recovery");
    auto identity = generate_stable_id();
    if (!identity)
        return identity.error();
    impl_->faulted = true;
    impl_->worker.reset(); // Joins; the old Store is destroyed on its worker.
    auto worker =
        detail::RuntimeWorker::open(impl_->runtime_path, *impl_->expected_store, impl_->world);
    if (!worker)
        return worker.error();
    auto registry = worker.value()->execute(detail::ReadRegistry{});
    if (!registry)
        return registry.error();
    const auto& records = std::get<DurableRegistry>(registry.value());
    auto sessions = hydrate(records, impl_->limits);
    if (!sessions)
        return sessions.error();
    std::vector<Impl::Binding> bindings;
    bindings.reserve(maximum_durable_characters);
    for (const auto& character : records.characters)
        bindings.push_back({character.account, character.character, character.entity});
    impl_->sessions = std::move(sessions).value();
    impl_->bindings.swap(bindings);
    impl_->worker = std::move(worker).value();
    impl_->runtime_id = RuntimeId{identity.value()};
    impl_->last_ticket = 0;
    impl_->faulted = false;
    return {};
}

} // namespace shark::storage
