#pragma once

#include "shark/storage/store.hpp"

namespace shark::server {
inline constexpr std::size_t region_journal_maximum_payload_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t region_journal_maximum_owners = 32;
enum class RegionJournalSlot { checkpoint, staged };
enum class FileDurability { file_flushed, file_and_directory_flushed, directory_flush_failed };
struct RegionJournalRecord {
    storage::StoreId store;
    WorldId world;
    RegionInstanceId instance;
    std::uint64_t tick;
    std::uint64_t route_watermark;
    std::vector<storage::RegionWriter> owners;
    // Empty for a normal checkpoint; otherwise one exact source request for
    // every owner. Every cut_digest equals SHA256 of the actual image payload.
    std::vector<storage::RegionTransferRequest> requests;
    std::vector<std::byte> payload;
    bool operator==(const RegionJournalRecord&) const = default;
};
struct RegionJournalSnapshot {
    RegionJournalRecord record;
    ContentDigest digest;
    ContentDigest payload_digest;
};
struct FilePublication {
    bool published;
    FileDurability durability;
    std::size_t bytes;
    ContentDigest digest;
    ContentDigest payload_digest;
};
// Trusted server-only synchronous storage, called outside native ticks. Exactly
// two fixed published slots and two reserved unpublished stages, under one
// exclusive creating-thread directory owner. No Tools/source/JSON dependency.
class RegionJournal final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RegionJournal>>
    open(const std::filesystem::path& directory, storage::StoreId store, WorldId world);
    ~RegionJournal();
    RegionJournal(const RegionJournal&) = delete;
    RegionJournal& operator=(const RegionJournal&) = delete;
    [[nodiscard]] Result<std::optional<RegionJournalSnapshot>> read(RegionJournalSlot slot) const;
    // Null means create-only. Replacing a slot requires its exact observed
    // envelope digest. The coordinator keeps staged bytes immutable until SQL
    // settles and a checkpoint covers them; the file service cannot infer that.
    [[nodiscard]] Result<FilePublication> publish(RegionJournalSlot slot,
                                                  const RegionJournalRecord& record,
                                                  std::optional<ContentDigest> expected = {});

  private:
    struct Impl;
    explicit RegionJournal(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace shark::server
