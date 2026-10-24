#pragma once

#include "shark/error.hpp"
#include "shark/name.hpp"
#include "shark/net/session.hpp"
#include "shark/scene_identity.hpp"
#include "shark/world_position.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace shark::storage {

struct StoreIdDomain;
using StoreId = TypedId<StoreIdDomain>;

inline constexpr std::uint32_t store_schema_version = 2;
inline constexpr std::uint64_t maximum_store_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t maximum_durable_accounts = 64;
inline constexpr std::size_t maximum_durable_characters = 128;
inline constexpr std::size_t maximum_durable_certificates = 128;
inline constexpr std::size_t maximum_characters_per_account = 8;
inline constexpr std::uint64_t maximum_durable_revision = 0x7fffffffffffffffULL;

struct StoreMetadata {
    StoreId id;
    WorldId world;
    std::uint32_t schema_version;
    bool operator==(const StoreMetadata&) const = default;
};

// A bounded checkpoint of server-accepted, non-economic state. Names are display
// text; position acceptance belongs to server simulation, never a player snapshot.
struct CharacterState {
    Name name;
    WorldPosition position;
    bool operator==(const CharacterState&) const = default;
};
struct AccountRecord {
    net::AccountId account;
    bool revoked;
    std::uint64_t roster_revision;
    bool operator==(const AccountRecord&) const = default;
};
struct EnrollmentRecord {
    net::AccountId account;
    net::CertificateFingerprint certificate;
    bool revoked;
    bool operator==(const EnrollmentRecord&) const = default;
};
struct CharacterRecord {
    net::AccountId account;
    net::CharacterId character;
    WorldEntityId entity;
    std::uint64_t revision;
    CharacterState state;
    bool operator==(const CharacterRecord&) const = default;
};
struct DurableRegistry {
    std::vector<AccountRecord> accounts;
    std::vector<EnrollmentRecord> certificates;
    std::vector<CharacterRecord> characters;
    bool operator==(const DurableRegistry&) const = default;
};
struct CharacterRoster {
    net::AccountId account;
    std::uint64_t revision;
    std::vector<CharacterRecord> characters;
    bool operator==(const CharacterRoster&) const = default;
};

[[nodiscard]] Result<void> validate_character_state(const CharacterState& state, WorldId world);

// Server-only, creating-thread owner of a real local database. No public SQL or
// native database handle. Parent directory must exist on local storage. Callers
// coordinate external file/topology changes; this is not a filesystem sandbox.
// SQLite's exclusive lock excludes cooperating connections until destruction.
class Store final {
  public:
    // Reserves a new file exclusively, then commits schema and identity together.
    // Never overwrites an existing path. Failed creation may leave an allocated
    // uninitialized file: inspect/remove it explicitly before another create.
    [[nodiscard]] static Result<std::unique_ptr<Store>> create(const std::filesystem::path& path,
                                                               WorldId world);
    // Never creates or migrates. Requires the exact supported database schema
    // and expected world. Foreign/future headers reject before SQLite can write.
    [[nodiscard]] static Result<std::unique_ptr<Store>> open(const std::filesystem::path& path,
                                                             WorldId expected_world);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    // Owned snapshot. Wrong-thread access and ownership contention are conflict;
    // malformed/incompatible storage is invalid_configuration; I/O is io_failure.
    [[nodiscard]] Result<StoreMetadata> metadata() const;

    // Trusted server storage operations, not player authorization. The sole
    // CharacterAuthority owner supplies authenticated admission. Records and
    // revocation tombstones remain bounded and are never removed/re-enrolled.
    [[nodiscard]] Result<DurableRegistry> registry() const;
    [[nodiscard]] Result<AccountRecord> provision_account(net::AccountId account);
    [[nodiscard]] Result<void> enroll_certificate(net::AccountId account,
                                                  const net::CertificateFingerprint& certificate);
    [[nodiscard]] Result<void> revoke_account(net::AccountId account);
    [[nodiscard]] Result<void> revoke_certificate(const net::CertificateFingerprint& certificate);
    [[nodiscard]] Result<CharacterRoster> roster(net::AccountId account) const;
    [[nodiscard]] Result<CharacterRecord> character(net::AccountId account,
                                                    net::CharacterId character) const;
    // Compare the roster revision before generating and committing both stable
    // character/entity IDs. A lost reply requires re-reading the roster, not an
    // unconditional create retry. New roster and character revisions start at 1.
    [[nodiscard]] Result<CharacterRecord> create_character(net::AccountId account,
                                                           std::uint64_t expected_roster_revision,
                                                           const CharacterState& initial);
    // Atomic compare-and-save; success acknowledges COMMIT and revision + 1.
    // Failed/uncertain commits never acknowledge success and quiesce this owner;
    // destroy/reopen and inspect durable revisions before deciding any retry.
    [[nodiscard]] Result<CharacterRecord> save_character(net::AccountId account,
                                                         net::CharacterId character,
                                                         std::uint64_t expected_revision,
                                                         const CharacterState& accepted_state);

  private:
    struct Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static Result<std::unique_ptr<Store>> acquire(const std::filesystem::path& path,
                                                                WorldId world, bool create_new);
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage
