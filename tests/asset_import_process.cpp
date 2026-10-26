#include "shark/tools/asset_import.hpp"

#include "project_files_native.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{std::string{error_code_name(result.error().code)} + ":" +
                                 result.error().context.operation};
    return std::move(result).value();
}
template <typename T> T id(char digit) {
    std::string text = "50000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T{take(StableId::parse(text))};
}
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out)
        throw std::runtime_error{"source fixture write failed"};
}
detail::FilePhase phase(std::string_view value) {
    const std::array phases{std::pair{"staged", detail::FilePhase::staged},
                            std::pair{"written", detail::FilePhase::written},
                            std::pair{"flushed", detail::FilePhase::flushed},
                            std::pair{"before_publish", detail::FilePhase::before_publish},
                            std::pair{"published", detail::FilePhase::published}};
    for (const auto& [name, selected] : phases)
        if (value == name)
            return selected;
    throw std::runtime_error{"unknown phase"};
}
} // namespace
int run(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error{"expected mode root [phase]"};
        const std::string_view mode{argv[1]};
        const std::string root_text{argv[2]};
        const auto root = std::filesystem::path{std::u8string{root_text.begin(), root_text.end()}};
        auto files = take(ProjectFiles::open(root));
        const auto source = take(ProjectPath::parse("asset.bin"));
        const auto sidecar = take(asset_sidecar_path(source));
        if (mode == "seed") {
            write(root / "asset.bin", "Before");
            take(files->write_asset_metadata(sidecar,
                                             {id<ProjectId>('1'), id<AssetId>('a'), "bytes",
                                              "Shark authored process fixture", "MIT",
                                              "shark.bytes"},
                                             {}));
        }
        auto catalog = take(AssetCatalog::create(*files, id<ProjectId>('1')));
        const std::array inventory{sidecar};
        const auto catalog_revision = take(catalog->refresh(inventory, take(catalog->revision())));
        ByteAssetImporter bytes;
        const std::array<AssetImporter*, 1> importers{&bytes};
        auto cache = take(AssetImports::open(*files, *catalog, id<ProjectId>('1'), importers));
        const AssetTargetProfile target{"process-x64", AssetAudience::client};
        std::size_t recovered = 0;
        if (mode == "interrupt")
            write(root / "asset.bin", "After");
        if (mode == "recover")
            recovered = take(AssetImports::recover(*files));
        if (mode == "seed" || mode == "interrupt" || mode == "recover" || mode == "blocked") {
            auto prepared = take(cache->prepare(id<AssetId>('a'), target, catalog_revision,
                                                take(cache->revision())));
            auto selected = detail::FilePhase::staged;
            if (mode == "interrupt") {
                if (argc != 4)
                    throw std::runtime_error{"missing phase"};
                selected = phase(argv[3]);
                static_cast<void>(detail::set_file_test_control(
                    {detail::FileFault::none,
                     [](detail::FilePhase reached, void* context) noexcept {
                         if (reached == *static_cast<detail::FilePhase*>(context))
                             std::_Exit(77);
                     },
                     &selected}));
            }
            auto commit =
                cache->publish(std::move(prepared), catalog_revision, take(cache->revision()));
            if (mode == "blocked") {
                if (commit || commit.error().code != ErrorCode::conflict)
                    throw std::runtime_error{"orphan stage did not block publication"};
                std::cout << "{\"format\":\"shark.asset-import-process\",\"schema_version\":1,"
                             "\"status\":\"PASS\",\"state\":\"blocked\"}\n";
                return 0;
            }
            take(std::move(commit));
        } else if (mode != "inspect-before" && mode != "inspect-after")
            throw std::runtime_error{"unknown mode"};
        const auto status =
            take(cache->query(id<AssetId>('a'), target, catalog_revision, take(cache->revision())));
        if (!status.last_valid_key)
            throw std::runtime_error{"lost last valid binding"};
        const auto artifact = take(cache->read(*status.last_valid_key, take(cache->revision())));
        const std::string body{reinterpret_cast<const char*>(artifact.payload.data() + 36),
                               artifact.payload.size() - 36};
        const bool after = mode == "recover" || mode == "inspect-after";
        if (body != (after ? "After" : "Before") ||
            (after && status.state != AssetCacheState::fresh))
            throw std::runtime_error{"wrong retained payload/state"};
        std::cout << "{\"format\":\"shark.asset-import-process\",\"schema_version\":1,\"status\":"
                     "\"PASS\",\"payload\":\""
                  << body
                  << "\",\"fresh\":" << (status.state == AssetCacheState::fresh ? "true" : "false")
                  << ",\"recovered\":" << recovered << ",\"key\":\""
                  << status.last_valid_key->to_hex() << "\"}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return 1;
            std::string text(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size,
                                    nullptr, nullptr) != size)
                return 1;
            text.pop_back();
            values.push_back(std::move(text));
        }
        std::vector<char*> pointers;
        for (auto& value : values)
            pointers.push_back(value.data());
        return run(argc, pointers.data());
    } catch (...) {
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
