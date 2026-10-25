#pragma once

#include "shark/error.hpp"
#include "shark/name.hpp"
#include "shark/net/session.hpp"
#include "shark/scene_identity.hpp"
#include "shark/world_position.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace shark::storage {

struct StoreIdDomain;
using StoreId = TypedId<StoreIdDomain>;

inline constexpr std::uint32_t store_schema_version = 3;
inline constexpr std::uint64_t maximum_store_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t maximum_durable_accounts = 64;
inline constexpr std::size_t maximum_durable_characters = 128;
inline constexpr std::size_t maximum_durable_certificates = 128;
inline constexpr std::size_t maximum_characters_per_account = 8;
inline constexpr std::uint64_t maximum_durable_revision = 0x7fffffffffffffffULL;
inline constexpr std::uint64_t maximum_economy_amount = 1'000'000'000;
inline constexpr std::size_t retained_operation_receipts = 8;

struct StoreMetadata {
    StoreId id;
    WorldId world;
    std::uint32_t schema_version;
    bool operator==(const StoreMetadata&) const = default;
};

// A validated offline snapshot summary, not an authorization or freshness token.
// Maintenance revalidates complete contents rather than trusting these counts.
struct SnapshotInfo {
    StoreMetadata metadata;
    std::uint64_t bytes;
    std::size_t accounts;
    std::size_t certificates;
    std::size_t characters;
    std::size_t economies;
    std::size_t operation_streams;
    std::size_t receipts;
    bool operator==(const SnapshotInfo&) const = default;
};
struct MigrationResult {
    StoreMetadata before;
    StoreMetadata after;
    bool already_current;
    bool operator==(const MigrationResult&) const = default;
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

// A deliberately small conservation fixture, not a general inventory or item catalog.
struct EconomyState {
    std::uint64_t currency;
    std::uint64_t item_units;
    bool operator==(const EconomyState&) const = default;
};
struct EconomyRecord {
    net::AccountId account;
    net::CharacterId character;
    std::uint64_t revision;
    EconomyState state;
    bool operator==(const EconomyRecord&) const = default;
};
struct OperationId {
    StoreId store;
    net::AccountId actor;
    std::uint64_t sequence;
    bool operator==(const OperationId&) const = default;
};
struct TransferRequest {
    OperationId operation;
    net::CharacterId source;
    net::CharacterId target;
    std::uint64_t expected_source_revision;
    std::uint64_t expected_target_revision;
    EconomyState quantity;
    bool operator==(const TransferRequest&) const = default;
};
enum class TransferStatus : std::uint8_t {
    committed,
    stale_revision,
    insufficient_assets,
    capacity_exceeded,
    target_unavailable,
    revision_exhausted
};
struct TransferReceipt {
    TransferRequest request;
    TransferStatus status;
    std::optional<EconomyRecord> source;
    std::optional<EconomyRecord> target;
    bool operator==(const TransferReceipt&) const = default;
};
struct TransferResult {
    TransferReceipt receipt;
    bool replayed;
    bool operator==(const TransferResult&) const = default;
};
struct OperationStream {
    StoreId store;
    net::AccountId actor;
    std::uint64_t high_watermark;
    std::vector<TransferReceipt> receipts;
    bool operator==(const OperationStream&) const = default;
};
[[nodiscard]] Result<void> validate_economy_state(const EconomyState& state);
[[nodiscard]] Result<void> validate_transfer_request(const TransferRequest& request);

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
    // Explicit offline maintenance. Exact historical schemas 1/2 and current 3
    // are supported here; ordinary open remains current-schema-only. Inspection
    // is read-only and cannot recover a hot journal. Caller releases live owners.
    [[nodiscard]] static Result<SnapshotInfo> inspect_snapshot(const std::filesystem::path& path,
                                                               StoreId expected_store,
                                                               WorldId expected_world);
    // Both copy operations reserve a new destination in a different canonical
    // directory, never overwrite, and independently reopen/validate the copy.
    // Failed/interrupted copies may leave allocated files for explicit cleanup.
    [[nodiscard]] static Result<SnapshotInfo> backup(const std::filesystem::path& source,
                                                     const std::filesystem::path& destination,
                                                     StoreId expected_store,
                                                     WorldId expected_world);
    // Requires an independently validated backup of the exact pre-upgrade state.
    // Sequential 1->2->3 transforms commit together and never grant economy state.
    // A retry at schema 3 succeeds only when it exactly matches that transform.
    // Supported interrupted source transactions may recover before inspection;
    // newer headers reject before SQLite can write. No downgrade is supported.
    [[nodiscard]] static Result<MigrationResult>
    migrate(const std::filesystem::path& source, const std::filesystem::path& verified_backup,
            StoreId expected_store, WorldId expected_world);
    // Isolated same-schema candidate only, never active-state replacement.
    // An older snapshot must not resume authoritative use after acknowledged
    // later effects: restored economic high-water marks would also be older.
    [[nodiscard]] static Result<SnapshotInfo> restore(const std::filesystem::path& snapshot,
                                                      const std::filesystem::path& destination,
                                                      StoreId expected_store,
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

    // Trusted provisioning only, once per character. No arbitrary later grants.
    [[nodiscard]] Result<EconomyRecord> initialize_economy(net::AccountId account,
                                                           net::CharacterId character,
                                                           const EconomyState& initial);
    [[nodiscard]] Result<EconomyRecord> economy(net::AccountId account,
                                                net::CharacterId character) const;
    // Exact next sequence admits one durable terminal outcome. Retained equal
    // retries return that same outcome before current revisions are considered.
    // Mismatched, expired or gapped operation IDs reject without admission.
    [[nodiscard]] Result<TransferResult> transfer(const TransferRequest& request);
    // Owned, bounded trusted audit; receipts include committed business rejects.
    [[nodiscard]] Result<OperationStream> operation_stream(net::AccountId actor) const;

  private:
    struct Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static Result<std::unique_ptr<Store>> acquire(const std::filesystem::path& path,
                                                                WorldId world, bool create_new);
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage
