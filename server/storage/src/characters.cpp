#include "shark/storage/characters.hpp"

#include <limits>
#include <thread>
#include <utility>

namespace shark::storage {
namespace {
Error rejected(const char* operation, const char* message = "Character operation rejected",
               ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}
} // namespace

struct CharacterAuthority::Impl {
    std::unique_ptr<Store> store;
    std::unique_ptr<net::SessionAuthority> sessions;
    net::SessionLimits limits;
    WorldId world;
    std::thread::id thread;
    bool faulted{false};

    Result<void> check(const char* operation) const {
        if (std::this_thread::get_id() != thread) {
            return rejected(operation, "Character authority requires its creating thread");
        }
        if (faulted) {
            return rejected(operation, "Character authority requires reopen and reconciliation",
                            ErrorCode::execution_failed);
        }
        return {};
    }

    Error storage_error(Error error) {
        if (error.code == ErrorCode::io_failure || error.code == ErrorCode::execution_failed ||
            error.code == ErrorCode::invalid_configuration) {
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
                                         const char* operation) {
        if (auto checked = check(operation); !checked) {
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
    const auto& records = registry.value();
    if (records.accounts.size() > limits.accounts ||
        records.characters.size() > limits.characters ||
        records.certificates.size() > limits.certificates) {
        return rejected(operation, "Durable registry exceeds session admission capacity",
                        ErrorCode::invalid_configuration);
    }
    auto sessions = net::SessionAuthority::create(limits);
    if (!sessions) {
        return sessions.error();
    }
    for (const auto& account : records.accounts) {
        if (auto result = sessions.value()->provision_account(account.account); !result) {
            return result.error();
        }
    }
    for (const auto& character : records.characters) {
        if (auto result =
                sessions.value()->assign_character(character.account, character.character);
            !result) {
            return result.error();
        }
    }
    for (const auto& enrollment : records.certificates) {
        if (auto result =
                sessions.value()->enroll_certificate(enrollment.account, enrollment.certificate);
            !result) {
            return result.error();
        }
    }
    for (const auto& enrollment : records.certificates) {
        if (enrollment.revoked) {
            if (auto result = sessions.value()->revoke_certificate(enrollment.certificate);
                !result) {
                return result.error();
            }
        }
    }
    for (const auto& account : records.accounts) {
        if (account.revoked) {
            if (auto result = sessions.value()->revoke_account(account.account); !result) {
                return result.error();
            }
        }
    }
    auto impl = std::make_unique<Impl>(Impl{std::move(store), std::move(sessions).value(), limits,
                                            metadata.value().world, std::this_thread::get_id()});
    return std::unique_ptr<CharacterAuthority>(new CharacterAuthority(std::move(impl)));
}

Result<StoreMetadata> CharacterAuthority::metadata() const {
    if (auto result = impl_->check("storage.characters.metadata"); !result) {
        return result.error();
    }
    return impl_->store->metadata();
}

Result<void> CharacterAuthority::provision_account(net::AccountId account) {
    constexpr auto operation = "storage.characters.provision";
    if (auto result = impl_->check(operation); !result) {
        return result;
    }
    auto stats = impl_->sessions->stats();
    if (!stats) {
        return stats.error();
    }
    if (stats.value().accounts >= impl_->limits.accounts) {
        return rejected(operation, "Account admission capacity exhausted");
    }
    auto stored = impl_->store->provision_account(account);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->provision_account(account));
}

Result<void>
CharacterAuthority::enroll_certificate(net::AccountId account,
                                       const net::CertificateFingerprint& certificate) {
    constexpr auto operation = "storage.characters.enroll";
    if (auto result = impl_->check(operation); !result) {
        return result;
    }
    auto stats = impl_->sessions->stats();
    if (!stats) {
        return stats.error();
    }
    if (stats.value().certificates >= impl_->limits.certificates) {
        return rejected(operation, "Certificate admission capacity exhausted");
    }
    auto stored = impl_->store->enroll_certificate(account, certificate);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->enroll_certificate(account, certificate));
}

Result<void> CharacterAuthority::revoke_account(net::AccountId account) {
    if (auto result = impl_->check("storage.characters.revoke_account"); !result) {
        return result;
    }
    auto stored = impl_->store->revoke_account(account);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return impl_->publish(impl_->sessions->revoke_account(account));
}

Result<void>
CharacterAuthority::revoke_certificate(const net::CertificateFingerprint& certificate) {
    if (auto result = impl_->check("storage.characters.revoke_certificate"); !result) {
        return result;
    }
    auto stored = impl_->store->revoke_certificate(certificate);
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
    if (auto result = impl_->check("storage.characters.initialize_economy"); !result) {
        return result.error();
    }
    auto stored = impl_->store->initialize_economy(account, character, initial);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

Result<CharacterRoster> CharacterAuthority::roster(net::TlsTransport& transport,
                                                   net::AccountId account, MonotonicTime now) {
    if (auto result = impl_->check("storage.characters.roster"); !result) {
        return result.error();
    }
    auto admitted = impl_->sessions->authorize_account(transport, account, now);
    if (!admitted) {
        return admitted.error();
    }
    auto stored = impl_->store->roster(admitted.value());
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
    if (auto result = impl_->check(operation); !result) {
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
    auto stored =
        impl_->store->create_character(admitted.value(), expected_roster_revision, initial);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    if (auto result = impl_->publish(
            impl_->sessions->assign_character(admitted.value(), stored.value().character));
        !result) {
        return result.error();
    }
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
    auto stored = impl_->store->character(session.value().account, session.value().character);
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
    auto saved = impl_->store->save_character(session.value().account, session.value().character,
                                              expected_revision, accepted_state);
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
    auto stored = impl_->store->economy(session.value().account, session.value().character);
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
    auto stored = impl_->store->operation_stream(session.value().account);
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
    auto stored = impl_->store->transfer(request);
    if (!stored) {
        return impl_->storage_error(stored.error());
    }
    return stored;
}

} // namespace shark::storage
