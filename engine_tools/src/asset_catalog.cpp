#include "shark/tools/asset_catalog.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <thread>
#include <utility>

namespace shark::tools {
namespace {
struct Failure {
    Error error;
};
[[noreturn]] void fail(ErrorCode code, const char* message, std::string path = {},
                       std::string asset = {}) {
    throw Failure{Error{code, message, {.path = std::move(path), .asset_id = std::move(asset)}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed, "Asset catalog allocation failed", {operation}};
    } catch (...) {
        return Error{ErrorCode::execution_failed, "Asset catalog operation failed", {operation}};
    }
}
std::string folded(std::string_view text) {
    std::string result{text};
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    }
    return result;
}
void reserve_path(const ProjectPath& path, std::map<std::string, std::string>& files) {
    if (!files.emplace(folded(path.text()), path.text()).second) {
        fail(ErrorCode::conflict, "Duplicate or case-colliding asset path", path.text());
    }
}
} // namespace

struct AssetCatalog::Impl {
    ProjectFiles& files;
    ProjectId project;
    AssetCatalogRevision revision;
    std::thread::id owner{std::this_thread::get_id()};
    std::vector<AssetRecord> records;

    Impl(ProjectFiles& source, ProjectId id, StableId scope)
        : files(source), project(id), revision{scope, 0} {}
    void check_owner() const {
        if (owner != std::this_thread::get_id()) {
            fail(ErrorCode::conflict, "Asset catalog requires its owner thread");
        }
    }
    void check(AssetCatalogRevision expected) const {
        check_owner();
        if (revision != expected) {
            fail(ErrorCode::conflict, "Asset catalog revision or owner scope changed");
        }
    }
};

AssetCatalog::AssetCatalog(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AssetCatalog::~AssetCatalog() = default;

Result<std::unique_ptr<AssetCatalog>> AssetCatalog::create(ProjectFiles& files, ProjectId project) {
    return boundary<std::unique_ptr<AssetCatalog>>("asset.catalog.create", [&] {
        return std::unique_ptr<AssetCatalog>(
            new AssetCatalog(std::make_unique<Impl>(files, project, take(generate_stable_id()))));
    });
}
Result<AssetCatalogRevision> AssetCatalog::revision() const {
    return boundary<AssetCatalogRevision>("asset.catalog.revision", [&] {
        impl_->check_owner();
        return impl_->revision;
    });
}

Result<AssetCatalogRevision> AssetCatalog::refresh(std::span<const ProjectPath> sidecars,
                                                   AssetCatalogRevision expected) {
    return boundary<AssetCatalogRevision>("asset.catalog.refresh", [&] {
        impl_->check(expected);
        if (sidecars.size() > asset_maximum_catalog_entries) {
            fail(ErrorCode::invalid_argument, "Asset catalog entry limit is 4096");
        }
        std::map<std::string, std::string> paths;
        std::size_t bytes = 0;
        auto charge = [&](std::size_t amount) {
            if (amount > asset_maximum_catalog_bytes - bytes) {
                fail(ErrorCode::invalid_configuration, "Asset catalog input budget exceeds 16 MiB");
            }
            bytes += amount;
        };
        // Charge the complete inventory before parsing/copying any path. A
        // deeply nested oversized inventory must reject without expensive work.
        constexpr std::size_t suffix_bytes = std::string_view{".sharkasset"}.size();
        for (const auto& sidecar : sidecars) {
            const auto size = sidecar.text().size();
            charge(size + (size > suffix_bytes ? size - suffix_bytes : 0));
        }
        // Reject inventory/path conflicts before file reads or copying inputs.
        for (const auto& sidecar : sidecars) {
            const auto source = take(asset_source_path(sidecar));
            reserve_path(sidecar, paths);
            reserve_path(source, paths);
        }
        // A source/sidecar cannot also be another asset's directory.
        // Adjacent folded full paths suffice for common-directory spellings.
        // Retaining every prefix would amplify deep input into quadratic memory.
        auto previous = paths.end();
        for (auto current = paths.begin(); current != paths.end(); ++current) {
            const auto& [path, original] = *current;
            if (previous != paths.end()) {
                const auto common = std::mismatch(previous->first.begin(), previous->first.end(),
                                                  path.begin(), path.end())
                                        .second;
                const auto length = static_cast<std::size_t>(common - path.begin());
                const auto slash = path.substr(0, length).find_last_of('/');
                if (slash != std::string::npos &&
                    previous->second.compare(0, slash + 1, original, 0, slash + 1) != 0) {
                    fail(ErrorCode::conflict, "Asset directories have conflicting case spellings",
                         original);
                }
            }
            previous = current;
            const auto child = paths.lower_bound(path + '/');
            if (child != paths.end() && child->first.starts_with(path + '/')) {
                fail(ErrorCode::conflict, "Asset path overlaps another asset directory", original);
            }
        }
        std::vector<AssetRecord> candidate;
        candidate.reserve(sidecars.size());
        std::set<AssetId> ids;
        for (const auto& sidecar : sidecars) {
            auto read = take(impl_->files.read_asset_metadata(sidecar));
            const auto& metadata = read.source.document;
            const auto asset = metadata.asset.value().to_string();
            charge(read.revision.byte_count());
            if (metadata.project != impl_->project) {
                fail(ErrorCode::conflict, "Asset belongs to a different project", sidecar.text(),
                     asset);
            }
            if (!ids.insert(metadata.asset).second) {
                fail(ErrorCode::conflict,
                     "Duplicate asset ID; reconcile copied sidecars explicitly", sidecar.text(),
                     asset);
            }
            auto source = take(asset_source_path(sidecar));
            const auto present = take(impl_->files.asset_source_exists(source));
            candidate.push_back({std::move(read.source.document), std::move(source), sidecar,
                                 std::move(read.revision), present});
        }
        std::sort(candidate.begin(), candidate.end(),
                  [](const auto& a, const auto& b) { return a.metadata.asset < b.metadata.asset; });
        if (candidate == impl_->records)
            return impl_->revision;
        if (impl_->revision.value == std::numeric_limits<std::uint64_t>::max()) {
            fail(ErrorCode::conflict, "Asset catalog revision exhausted; create a new owner");
        }
        impl_->records.swap(candidate);
        ++impl_->revision.value;
        return impl_->revision;
    });
}

Result<AssetResolution> AssetCatalog::resolve(AssetId asset, AssetCatalogRevision expected) const {
    return boundary<AssetResolution>("asset.catalog.resolve", [&] {
        impl_->check(expected);
        const auto found = std::lower_bound(
            impl_->records.begin(), impl_->records.end(), asset,
            [](const AssetRecord& record, AssetId id) { return record.metadata.asset < id; });
        if (found == impl_->records.end() || found->metadata.asset != asset) {
            return AssetResolution{impl_->revision, asset, AssetAvailability::unknown_id, {}};
        }
        return AssetResolution{impl_->revision, asset,
                               found->source_present ? AssetAvailability::source_present
                                                     : AssetAvailability::source_missing,
                               *found};
    });
}

Result<AssetCatalogPage> AssetCatalog::query(AssetCatalogRevision expected, std::size_t offset,
                                             std::size_t page_size) const {
    return boundary<AssetCatalogPage>("asset.catalog.query", [&] {
        impl_->check(expected);
        if (page_size == 0 || page_size > asset_maximum_page_size ||
            offset > impl_->records.size()) {
            fail(ErrorCode::invalid_argument, "Invalid asset query page size or offset");
        }
        const auto end = offset + std::min(page_size, impl_->records.size() - offset);
        AssetCatalogPage page{impl_->revision, impl_->records.size(), {}, {}};
        page.records.reserve(end - offset);
        for (auto i = offset; i < end; ++i)
            page.records.push_back(impl_->records[i]);
        if (end != impl_->records.size())
            page.next_offset = end;
        return page;
    });
}

} // namespace shark::tools
