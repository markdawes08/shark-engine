#pragma once

#include "durable_test_hooks.hpp"
#include "shark/storage/store.hpp"

#include <memory>
#include <optional>
#include <variant>

namespace shark::storage::detail {

struct ReadMetadata {};
struct ReadRegistry {};
struct ProvisionAccount {
    net::AccountId account;
};
struct EnrollCertificate {
    net::AccountId account;
    net::CertificateFingerprint certificate;
};
struct RevokeAccount {
    net::AccountId account;
};
struct RevokeCertificate {
    net::CertificateFingerprint certificate;
};
struct ReadRoster {
    net::AccountId account;
};
struct ReadCharacter {
    net::AccountId account;
    net::CharacterId character;
};
struct CreateCharacter {
    net::AccountId account;
    std::uint64_t revision;
    CharacterState state;
};
struct SaveCharacter {
    net::AccountId account;
    net::CharacterId character;
    std::uint64_t revision;
    CharacterState state;
};
struct InitializeEconomy {
    net::AccountId account;
    net::CharacterId character;
    EconomyState state;
};
struct ReadEconomy {
    net::AccountId account;
    net::CharacterId character;
};
struct ReadOperationStream {
    net::AccountId account;
};
using StoreCommand =
    std::variant<ReadMetadata, ReadRegistry, ProvisionAccount, EnrollCertificate, RevokeAccount,
                 RevokeCertificate, ReadRoster, ReadCharacter, CreateCharacter, SaveCharacter,
                 InitializeEconomy, ReadEconomy, ReadOperationStream, TransferRequest>;
using StoreValue =
    std::variant<std::monostate, StoreMetadata, DurableRegistry, AccountRecord, CharacterRoster,
                 CharacterRecord, EconomyRecord, OperationStream, TransferResult>;
using StoreResult = Result<StoreValue>;
[[nodiscard]] StoreResult execute_store(Store& store, const StoreCommand& command);

struct WorkerToken {
    std::size_t slot;
    std::uint64_t sequence;
};
// Internal typed executor; no arbitrary callbacks, SQL or transport/Scene access.
class RuntimeWorker final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RuntimeWorker>>
    open(const std::filesystem::path& path, StoreId expected_store, WorldId world);
    ~RuntimeWorker();
    RuntimeWorker(const RuntimeWorker&) = delete;
    RuntimeWorker& operator=(const RuntimeWorker&) = delete;
    [[nodiscard]] StoreResult execute(StoreCommand command);
    [[nodiscard]] Result<void> submit(WorkerToken token, StoreCommand command);
    [[nodiscard]] std::optional<WorkerToken> poll();
    [[nodiscard]] StoreResult take(WorkerToken token);

  private:
    struct Impl;
    explicit RuntimeWorker(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage::detail
