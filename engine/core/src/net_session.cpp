#include "shark/net/session.hpp"

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace shark::net {
namespace {
Error rejection(const char* operation, ErrorCode code = ErrorCode::conflict) {
    return {code, "Session request rejected", {.operation = operation}};
}

struct Window {
    MonotonicTime start{0};
    std::uint32_t attempts{0};
};

bool spend(Window& window, MonotonicTime now, MonotonicTime duration, std::uint32_t maximum) {
    if (now - window.start >= duration) {
        window.start = now;
        window.attempts = 0;
    }
    if (window.attempts >= maximum) {
        return false;
    }
    ++window.attempts;
    return true;
}

bool fits_deadline(MonotonicTime now, MonotonicTime duration) {
    return now.count() <= std::numeric_limits<MonotonicTime::rep>::max() - duration.count();
}

bool valid_limits(const SessionLimits& limits) {
    using namespace std::chrono_literals;
    return limits.accounts > 0 && limits.accounts <= 1024 && limits.characters > 0 &&
           limits.characters <= 4096 && limits.certificates > 0 && limits.certificates <= 4096 &&
           limits.live_sessions > 0 && limits.live_sessions <= limits.accounts &&
           limits.login_attempts_per_window > 0 && limits.login_attempts_per_window <= 65536 &&
           limits.auth_attempts_per_window > 0 && limits.auth_attempts_per_window <= 4096 &&
           limits.commands_per_window > 0 && limits.commands_per_window <= 65536 &&
           limits.auth_window > 0ns && limits.auth_window <= 1h && limits.command_window > 0ns &&
           limits.command_window <= 1h && limits.session_lifetime > 0ns &&
           limits.session_lifetime <= 24h && limits.session_idle_timeout > 0ns &&
           limits.session_idle_timeout <= limits.session_lifetime;
}
} // namespace

struct SessionAuthority::Impl {
    struct Account {
        AccountId id;
        bool revoked{false};
        std::uint64_t epoch{0};
        std::uint64_t last_connection{0};
        Window rate{};
    };
    struct Character {
        CharacterId id;
        AccountId account;
    };
    struct Enrollment {
        CertificateFingerprint certificate;
        AccountId account;
        bool revoked{false};
        Window rate{};
    };
    struct Session {
        SessionSnapshot snapshot;
        PeerIdentity peer;
        MonotonicTime lifetime_deadline;
        MonotonicTime idle_deadline;
        Window rate{};
    };

    explicit Impl(SessionLimits value) : limits(value) {
        accounts.reserve(limits.accounts);
        characters.reserve(limits.characters);
        certificates.reserve(limits.certificates);
        sessions.reserve(limits.live_sessions);
    }

    Result<void> check_owner() const {
        if (std::this_thread::get_id() != owner) {
            return rejection("net.session.owner");
        }
        return {};
    }

    Result<std::size_t> advance(MonotonicTime now) {
        if (now < MonotonicTime::zero() || now < last_time) {
            return rejection("net.session.time", ErrorCode::invalid_argument);
        }
        last_time = now;
        return std::erase_if(sessions, [now](const Session& session) {
            return now >= session.lifetime_deadline || now >= session.idle_deadline;
        });
    }

    Result<SessionSnapshot> login(const PeerIdentity& peer, const LoginRequest& request,
                                  MonotonicTime now) {
        auto certificate =
            std::ranges::find(certificates, peer.certificate, &Enrollment::certificate);
        if (certificate == certificates.end()) {
            return rejection("net.session.enrollment");
        }
        auto account = std::ranges::find(accounts, certificate->account, &Account::id);
        // Every enrollment is created against an existing, retained account.
        if (account == accounts.end()) {
            return rejection("net.session.enrollment");
        }
        const bool certificate_budget =
            spend(certificate->rate, now, limits.auth_window, limits.auth_attempts_per_window);
        const bool account_budget =
            spend(account->rate, now, limits.auth_window, limits.auth_attempts_per_window);
        if (!certificate_budget || !account_budget) {
            return rejection(certificate_budget ? "net.session.rate.account"
                                                : "net.session.rate.certificate");
        }
        if (certificate->revoked || account->revoked) {
            return rejection("net.session.revoked");
        }
        auto character = std::ranges::find(characters, request.character, &Character::id);
        if (request.account != account->id || character == characters.end() ||
            character->account != account->id) {
            return rejection("net.session.ownership");
        }
        if (peer.connection_id == 0 || peer.connection_id <= account->last_connection) {
            return rejection("net.session.reconnect");
        }
        auto existing = std::ranges::find(
            sessions, account->id, [](const Session& session) { return session.snapshot.account; });
        if (existing == sessions.end() && sessions.size() >= limits.live_sessions) {
            return rejection("net.session.capacity");
        }
        if (!fits_deadline(now, limits.session_lifetime) ||
            !fits_deadline(now, limits.session_idle_timeout)) {
            return rejection("net.session.time", ErrorCode::invalid_argument);
        }
        auto epoch = next_ownership_epoch(account->epoch);
        if (!epoch) {
            return epoch.error();
        }
        auto id = generate_stable_id();
        if (!id) {
            return rejection("net.session.entropy", id.error().code);
        }
        const SessionId session_id{id.value()};
        if (std::ranges::any_of(sessions, [&](const Session& session) {
                return session.snapshot.session == session_id;
            })) {
            return rejection("net.session.entropy");
        }
        Session candidate{{session_id, account->id, character->id, epoch.value(), 0, false, true},
                          peer,
                          now + limits.session_lifetime,
                          now + limits.session_idle_timeout,
                          {now, 0}};
        // No operation below allocates: capacity was reserved at construction.
        // Publish ownership only after complete validation and OS entropy succeed.
        if (existing == sessions.end()) {
            sessions.push_back(candidate);
        } else {
            *existing = candidate;
        }
        account->epoch = epoch.value();
        account->last_connection = peer.connection_id;
        return candidate.snapshot;
    }

    Result<SessionSnapshot> command(const PeerIdentity& peer, const SessionRequest& request,
                                    const SessionEnvelope& envelope, MonotonicTime now) {
        auto session = std::ranges::find(sessions, peer.connection_id, [](const Session& value) {
            return value.peer.connection_id;
        });
        if (session == sessions.end() || session->peer.certificate != peer.certificate) {
            return rejection("net.session.connection");
        }
        if (!spend(session->rate, now, limits.command_window, limits.commands_per_window)) {
            return rejection("net.session.rate.command");
        }
        if (session->snapshot.session != envelope.session ||
            session->snapshot.character != envelope.character ||
            session->snapshot.ownership_epoch != envelope.ownership_epoch) {
            return rejection("net.session.ownership");
        }
        auto sequence = check_sequence(session->snapshot.last_sequence, envelope.sequence);
        if (!sequence) {
            return rejection("net.session.sequence");
        }
        const bool logout = std::holds_alternative<LogoutRequest>(request);
        if (!logout && !fits_deadline(now, limits.session_idle_timeout)) {
            return rejection("net.session.time", ErrorCode::invalid_argument);
        }
        SessionSnapshot accepted = session->snapshot;
        accepted.last_sequence = sequence.value();
        if (const auto* ready = std::get_if<SetReadyRequest>(&request)) {
            accepted.ready = ready->ready;
        }
        if (logout) {
            accepted.ready = false;
            accepted.active = false;
            sessions.erase(session);
        } else {
            session->snapshot = accepted;
            session->idle_deadline = now + limits.session_idle_timeout;
        }
        return accepted;
    }

    SessionLimits limits;
    const std::thread::id owner{std::this_thread::get_id()};
    MonotonicTime last_time{0};
    Window login_rate{};
    std::vector<Account> accounts;
    std::vector<Character> characters;
    std::vector<Enrollment> certificates;
    std::vector<Session> sessions;
};

SessionReply public_session_reply(const Result<SessionSnapshot>& result) {
    if (result) {
        return result.value();
    }
    return SessionRejected{};
}

Result<std::uint64_t> next_ownership_epoch(std::uint64_t previous) {
    if (previous == std::numeric_limits<std::uint64_t>::max()) {
        return rejection("net.session.epoch");
    }
    return previous + 1;
}

SessionAuthority::SessionAuthority(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SessionAuthority::~SessionAuthority() = default;

Result<std::unique_ptr<SessionAuthority>> SessionAuthority::create(const SessionLimits& limits) {
    if (!valid_limits(limits)) {
        return rejection("net.session.create", ErrorCode::invalid_configuration);
    }
    return std::unique_ptr<SessionAuthority>{new SessionAuthority{std::make_unique<Impl>(limits)}};
}

Result<void> SessionAuthority::provision_account(AccountId account) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (impl_->accounts.size() >= impl_->limits.accounts ||
        std::ranges::find(impl_->accounts, account, &Impl::Account::id) != impl_->accounts.end()) {
        return rejection("net.session.provision-account");
    }
    impl_->accounts.push_back({account});
    return {};
}

Result<void> SessionAuthority::assign_character(AccountId account, CharacterId character) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    auto found = std::ranges::find(impl_->accounts, account, &Impl::Account::id);
    if (found == impl_->accounts.end() || found->revoked ||
        impl_->characters.size() >= impl_->limits.characters ||
        std::ranges::find(impl_->characters, character, &Impl::Character::id) !=
            impl_->characters.end()) {
        return rejection("net.session.assign-character");
    }
    impl_->characters.push_back({character, account});
    return {};
}

Result<void> SessionAuthority::enroll_certificate(AccountId account,
                                                  const CertificateFingerprint& certificate) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    auto found = std::ranges::find(impl_->accounts, account, &Impl::Account::id);
    if (found == impl_->accounts.end() || found->revoked ||
        impl_->certificates.size() >= impl_->limits.certificates ||
        std::ranges::find(impl_->certificates, certificate, &Impl::Enrollment::certificate) !=
            impl_->certificates.end()) {
        return rejection("net.session.enroll-certificate");
    }
    impl_->certificates.push_back({certificate, account});
    return {};
}

Result<void> SessionAuthority::revoke_account(AccountId account) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    auto found = std::ranges::find(impl_->accounts, account, &Impl::Account::id);
    if (found == impl_->accounts.end()) {
        return rejection("net.session.revoke-account");
    }
    found->revoked = true;
    std::erase_if(impl_->sessions, [account](const Impl::Session& session) {
        return session.snapshot.account == account;
    });
    return {};
}

Result<void> SessionAuthority::revoke_certificate(const CertificateFingerprint& certificate) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    auto found =
        std::ranges::find(impl_->certificates, certificate, &Impl::Enrollment::certificate);
    if (found == impl_->certificates.end()) {
        return rejection("net.session.revoke-certificate");
    }
    found->revoked = true;
    std::erase_if(impl_->sessions, [&](const Impl::Session& session) {
        return session.peer.certificate == certificate;
    });
    return {};
}

Result<bool> SessionAuthority::revoke_session(SessionId session) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    return std::erase_if(impl_->sessions, [session](const Impl::Session& value) {
               return value.snapshot.session == session;
           }) != 0;
}

Result<SessionSnapshot> SessionAuthority::apply(TlsTransport& transport,
                                                const SessionRequest& request, MonotonicTime now) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (auto time = impl_->advance(now); !time) {
        return time.error();
    }
    const auto* login = std::get_if<LoginRequest>(&request);
    if (login && !spend(impl_->login_rate, now, impl_->limits.auth_window,
                        impl_->limits.login_attempts_per_window)) {
        return rejection("net.session.rate.authority");
    }
    auto peer = transport.authenticated_peer();
    if (!peer || peer.value().connection_id == 0) {
        return rejection("net.session.peer");
    }
    if (login) {
        return impl_->login(peer.value(), *login, now);
    }
    if (const auto* ready = std::get_if<SetReadyRequest>(&request)) {
        return impl_->command(peer.value(), request, ready->authority, now);
    }
    if (const auto* heartbeat = std::get_if<HeartbeatRequest>(&request)) {
        return impl_->command(peer.value(), request, heartbeat->authority, now);
    }
    if (const auto* logout = std::get_if<LogoutRequest>(&request)) {
        return impl_->command(peer.value(), request, logout->authority, now);
    }
    return rejection("net.session.request", ErrorCode::invalid_argument);
}

Result<std::size_t> SessionAuthority::expire(MonotonicTime now) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    return impl_->advance(now);
}

Result<SessionStats> SessionAuthority::stats() const {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    return SessionStats{impl_->accounts.size(), impl_->characters.size(),
                        impl_->certificates.size(), impl_->sessions.size()};
}

} // namespace shark::net
