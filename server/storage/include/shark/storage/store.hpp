#pragma once

#include "shark/error.hpp"
#include "shark/scene_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace shark::storage {

struct StoreIdDomain;
using StoreId = TypedId<StoreIdDomain>;

inline constexpr std::uint32_t store_schema_version = 1;
inline constexpr std::uint64_t maximum_store_bytes = 16 * 1024 * 1024;

struct StoreMetadata {
    StoreId id;
    WorldId world;
    std::uint32_t schema_version;
    bool operator==(const StoreMetadata&) const = default;
};

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

  private:
    struct Impl;
    explicit Store(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] static Result<std::unique_ptr<Store>> acquire(const std::filesystem::path& path,
                                                                WorldId world, bool create_new);
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::storage
