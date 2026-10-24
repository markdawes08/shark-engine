#include <shark/storage/store.hpp>

#include "storage_recovery_vfs.hpp"
#include "store_test_hooks.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::storage;
namespace fs = std::filesystem;

void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error{message};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ":" +
                                 std::string{error_code_name(result.error().code)}};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ":" +
                                 std::string{error_code_name(result.error().code)}};
}
StableId identity(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return take(StableId::parse(text.str()));
}
net::AccountId account(unsigned value = 1) {
    return net::AccountId{identity(value)};
}
WorldId world() {
    return WorldId{identity(900)};
}
net::CertificateFingerprint certificate(unsigned value = 1) {
    net::CertificateFingerprint result{};
    result.back() = static_cast<std::byte>(value);
    return result;
}
std::string certificate_text(const net::CertificateFingerprint& value) {
    std::ostringstream text;
    for (const auto byte : value)
        text << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(byte);
    return text.str();
}
CharacterState state(std::string name) {
    const bool saved = name == "Saved";
    return {Name{std::move(name)},
            WorldPosition{world(), {saved ? 9 : 1, 2, 3}, {0.0, 1.5, 1023.75}}};
}
net::CharacterId character(Store& owner, bool second = false) {
    const auto roster = take(owner.roster(account(second ? 2 : 1)));
    const auto found =
        std::find_if(roster.characters.begin(), roster.characters.end(),
                     [](const auto& row) { return row.state.name.value != "Created"; });
    check(found != roster.characters.end(), "Seed character missing");
    return found->character;
}
bool transfer(std::string_view mutation) {
    return mutation.starts_with("transfer");
}
bool pruned(std::string_view mutation) {
    return mutation == "transfer-prune" || mutation == "transfer-prune-reject";
}
bool business_reject(std::string_view mutation) {
    return mutation == "transfer-reject" || mutation == "transfer-prune-reject";
}
std::string_view status_name(TransferStatus status) {
    switch (status) {
    case TransferStatus::committed:
        return "committed";
    case TransferStatus::stale_revision:
        return "stale_revision";
    case TransferStatus::insufficient_assets:
        return "insufficient_assets";
    case TransferStatus::capacity_exceeded:
        return "capacity_exceeded";
    case TransferStatus::target_unavailable:
        return "target_unavailable";
    case TransferStatus::revision_exhausted:
        return "revision_exhausted";
    }
    throw std::runtime_error{"Unknown transfer status"};
}
void print_economy(const EconomyRecord& row) {
    std::cout << "{\"account\":\"" << row.account.value().to_string() << "\",\"character\":\""
              << row.character.value().to_string() << "\",\"revision\":" << row.revision
              << ",\"currency\":" << row.state.currency << ",\"items\":" << row.state.item_units
              << '}';
}
void print_receipt(const TransferReceipt& row) {
    const auto& request = row.request;
    std::cout << "{\"store\":\"" << request.operation.store.value().to_string() << "\",\"actor\":\""
              << request.operation.actor.value().to_string()
              << "\",\"sequence\":" << request.operation.sequence << ",\"source\":\""
              << request.source.value().to_string() << "\",\"target\":\""
              << request.target.value().to_string()
              << "\",\"expected_source_revision\":" << request.expected_source_revision
              << ",\"expected_target_revision\":" << request.expected_target_revision
              << ",\"currency\":" << request.quantity.currency
              << ",\"items\":" << request.quantity.item_units << ",\"status\":\""
              << status_name(row.status) << "\",\"source_record\":";
    if (row.source)
        print_economy(*row.source);
    else
        std::cout << "null";
    std::cout << ",\"target_record\":";
    if (row.target)
        print_economy(*row.target);
    else
        std::cout << "null";
    std::cout << '}';
}
void print_snapshot(Store& owner) {
    const auto metadata = take(owner.metadata());
    const auto registry = take(owner.registry());
    std::cout << "{\"store\":\"" << metadata.id.value().to_string() << "\",\"world\":\""
              << metadata.world.value().to_string() << "\",\"schema\":" << metadata.schema_version
              << ",\"accounts\":[";
    bool comma = false;
    for (const auto& row : registry.accounts) {
        if (std::exchange(comma, true))
            std::cout << ',';
        std::cout << "{\"id\":\"" << row.account.value().to_string()
                  << "\",\"revoked\":" << (row.revoked ? "true" : "false")
                  << ",\"roster_revision\":" << row.roster_revision << '}';
    }
    std::cout << "],\"certificates\":[";
    comma = false;
    for (const auto& row : registry.certificates) {
        if (std::exchange(comma, true))
            std::cout << ',';
        std::cout << "{\"account\":\"" << row.account.value().to_string() << "\",\"fingerprint\":\""
                  << certificate_text(row.certificate)
                  << "\",\"revoked\":" << (row.revoked ? "true" : "false") << '}';
    }
    std::cout << "],\"characters\":[";
    comma = false;
    for (const auto& row : registry.characters) {
        if (std::exchange(comma, true))
            std::cout << ',';
        std::cout << "{\"account\":\"" << row.account.value().to_string() << "\",\"id\":\""
                  << row.character.value().to_string() << "\",\"entity\":\""
                  << row.entity.value().to_string() << "\",\"revision\":" << row.revision
                  << ",\"name\":\"" << row.state.name.value << "\",\"world\":\""
                  << row.state.position.world.value().to_string() << "\",\"cell\":["
                  << row.state.position.cell.x << ',' << row.state.position.cell.y << ','
                  << row.state.position.cell.z << "],\"offset\":[" << row.state.position.offset[0]
                  << ',' << row.state.position.offset[1] << ',' << row.state.position.offset[2]
                  << "]}";
    }
    std::cout << "],\"economies\":[";
    comma = false;
    for (const auto& row : registry.characters) {
        const auto economy = owner.economy(row.account, row.character);
        if (!economy) {
            check(economy.error().code == ErrorCode::conflict, "Unexpected economy read error");
            continue;
        }
        if (std::exchange(comma, true))
            std::cout << ',';
        print_economy(economy.value());
    }
    std::cout << "],\"streams\":[";
    comma = false;
    for (const auto& row : registry.accounts) {
        const auto stream = take(owner.operation_stream(row.account));
        if (std::exchange(comma, true))
            std::cout << ',';
        std::cout << "{\"actor\":\"" << stream.actor.value().to_string()
                  << "\",\"high_watermark\":" << stream.high_watermark << ",\"receipts\":[";
        bool receipt_comma = false;
        for (const auto& receipt : stream.receipts) {
            if (std::exchange(receipt_comma, true))
                std::cout << ',';
            print_receipt(receipt);
        }
        std::cout << "]}";
    }
    std::cout << "]}";
}
struct Outcome {
    bool ok;
    std::string code;
    std::string operation;
    std::optional<TransferResult> transferred{};
    int sqlite_code{0};
};
template <typename T> Outcome outcome(Result<T> result) {
    if (!result) {
        Outcome failed{false, std::string{error_code_name(result.error().code)},
                       result.error().context.operation};
        constexpr std::string_view marker = "(SQLite code ";
        const auto position = result.error().message.find(marker);
        if (position != std::string::npos)
            failed.sqlite_code = std::stoi(result.error().message.substr(position + marker.size()));
        return failed;
    }
    return {true, "", ""};
}
Outcome invoke(Store& owner, std::string_view mutation) {
    if (mutation == "provision")
        return outcome(owner.provision_account(account(3)));
    if (mutation == "enroll")
        return outcome(owner.enroll_certificate(account(), certificate(3)));
    if (mutation == "revoke-account")
        return outcome(owner.revoke_account(account()));
    if (mutation == "revoke-certificate")
        return outcome(owner.revoke_certificate(certificate()));
    if (mutation == "create-character")
        return outcome(owner.create_character(account(), 2, state("Created")));
    const auto source = character(owner);
    if (mutation == "save-character")
        return outcome(owner.save_character(account(), source, 1, state("Saved")));
    if (mutation == "initialize-economy")
        return outcome(owner.initialize_economy(account(), source, {1000, 100}));
    check(transfer(mutation), "Unknown mutation");
    const TransferRequest request{
        {take(owner.metadata()).id, account(), pruned(mutation) ? 9U : 1U},
        source,
        character(owner, true),
        pruned(mutation) ? 9U : 1U,
        pruned(mutation) ? 9U : 1U,
        {business_reject(mutation) ? 1001U : 7U, 2}};
    auto result = owner.transfer(request);
    auto output = outcome(result);
    if (result)
        output.transferred = std::move(result).value();
    return output;
}
void print_outcome(const Outcome& result) {
    std::cout << "{\"ok\":" << (result.ok ? "true" : "false") << ",\"code\":\"" << result.code
              << "\",\"operation\":\"" << result.operation
              << "\",\"sqlite_code\":" << result.sqlite_code << ",\"replayed\":";
    if (result.transferred)
        std::cout << (result.transferred->replayed ? "true" : "false");
    else
        std::cout << "null";
    std::cout << ",\"receipt\":";
    if (result.transferred)
        print_receipt(result.transferred->receipt);
    else
        std::cout << "null";
    std::cout << '}';
}
void seed(const fs::path& path, std::string_view mutation) {
    auto owner = take(Store::create(path, world()));
    take(owner->provision_account(account()));
    take(owner->provision_account(account(2)));
    take(owner->enroll_certificate(account(), certificate()));
    take(owner->enroll_certificate(account(2), certificate(2)));
    const auto source = take(owner->create_character(account(), 1, state("Source"))).character;
    const auto target = take(owner->create_character(account(2), 1, state("Target"))).character;
    if (mutation != "initialize-economy")
        take(owner->initialize_economy(account(), source, {1000, 100}));
    take(owner->initialize_economy(account(2), target, {200, 20}));
    if (pruned(mutation)) {
        for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) {
            const TransferRequest request{{take(owner->metadata()).id, account(), sequence},
                                          source,
                                          target,
                                          sequence,
                                          sequence,
                                          {1, 1}};
            check(take(owner->transfer(request)).receipt.status == TransferStatus::committed,
                  "Cannot seed retained transfer window");
        }
    }
    std::cout << "{\"status\":\"PASS\",\"event\":\"seed\",\"snapshot\":";
    print_snapshot(*owner);
    std::cout << "}\n";
}
detail::WritePhase requested_phase = detail::WritePhase::after_write;
std::string_view current_mutation;
std::string_view current_phase;
bool interrupt(detail::WritePhase phase) noexcept {
    if (phase == requested_phase) {
        std::cout << "{\"status\":\"PASS\",\"event\":\"boundary\",\"mutation\":\""
                  << current_mutation << "\",\"phase\":\"" << current_phase << "\"}\n"
                  << std::flush;
        std::_Exit(73);
    }
    return false;
}
void mutate(const fs::path& path, std::string_view mutation, std::string_view phase,
            test::RecoveryVfs& vfs) {
    auto owner = take(Store::open(path, world()));
    const bool outage = phase.ends_with("-outage");
    if (outage) {
        check(phase == "write-outage" || phase == "sync-outage" || phase == "full-outage",
              "Unknown outage phase");
        const auto fault = phase == "write-outage"  ? test::IoFault::write
                           : phase == "sync-outage" ? test::IoFault::sync
                                                    : test::IoFault::full;
        vfs.arm(fault);
        const auto result = invoke(*owner, mutation);
        const auto hits = vfs.failures();
        check(!result.ok && result.code == "io_failure" && hits != 0,
              "Actual SQLite I/O outage did not reject mutation");
        const auto guarded = owner->metadata();
        const bool fenced = !guarded && guarded.error().code == ErrorCode::execution_failed;
        if (phase == "sync-outage")
            check(fenced, "Commit outage did not fence owner");
        vfs.clear();
        owner.reset();
        std::cout << "{\"status\":\"PASS\",\"event\":\"outage\",\"mutation\":\"" << mutation
                  << "\",\"phase\":\"" << phase << "\",\"injected_calls\":" << hits
                  << ",\"owner_fenced\":" << (fenced ? "true" : "false") << ",\"outcome\":";
        print_outcome(result);
        std::cout << "}\n";
        return;
    }
    if (phase != "after_reply") {
        current_mutation = mutation;
        current_phase = phase;
        if (phase == "after_write")
            requested_phase = detail::WritePhase::after_write;
        else if (phase == "before_commit")
            requested_phase = detail::WritePhase::before_commit;
        else if (phase == "after_commit")
            requested_phase = detail::WritePhase::after_commit;
        else if (phase == "after_source_debit")
            requested_phase = detail::WritePhase::after_source_debit;
        else
            throw std::runtime_error{"Unknown crash phase"};
        detail::set_write_fault_hook(interrupt);
    }
    const auto result = invoke(*owner, mutation);
    check(result.ok, "Mutation failed before the requested crash boundary");
    check(phase == "after_reply", "Requested crash boundary was not reached");
    std::cout << "{\"status\":\"PASS\",\"event\":\"ack\",\"mutation\":\"" << mutation
              << "\",\"phase\":\"" << phase << "\",\"outcome\":";
    print_outcome(result);
    std::cout << ",\"snapshot\":";
    print_snapshot(*owner);
    std::cout << "}\n" << std::flush;
    std::_Exit(73);
}
void reconcile(const fs::path& path, std::string_view mutation) {
    auto owner = take(Store::open(path, world()));
    std::cout << "{\"status\":\"PASS\",\"event\":\"reconcile\",\"before\":";
    print_snapshot(*owner);
    const auto retried = invoke(*owner, mutation);
    std::cout << ",\"retry\":";
    print_outcome(retried);
    std::cout << ",\"after\":";
    print_snapshot(*owner);
    owner.reset();
    owner = take(Store::open(path, world()));
    std::cout << ",\"reopened\":";
    print_snapshot(*owner);
    std::cout << "}\n";
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc == 4 || argc == 5, "Expected mode mutation [phase] database");
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            const auto value = fs::path{argv[index]}.u8string();
            args.emplace_back(value.begin(), value.end());
        }
        const std::array<std::string_view, 11> mutations{
            "provision",        "enroll",         "revoke-account",       "revoke-certificate",
            "create-character", "save-character", "initialize-economy",   "transfer",
            "transfer-reject",  "transfer-prune", "transfer-prune-reject"};
        check(std::find(mutations.begin(), mutations.end(), args[1]) != mutations.end(),
              "Unknown mutation selection");
        const auto path = fs::path{std::u8string{args.back().begin(), args.back().end()}};
        test::RecoveryVfs vfs;
        if (args[0] == "seed" && argc == 4)
            seed(path, args[1]);
        else if (args[0] == "mutate" && argc == 5)
            mutate(path, args[1], args[2], vfs);
        else if (args[0] == "reconcile" && argc == 4)
            reconcile(path, args[1]);
        else
            throw std::runtime_error{"Unknown recovery mode"};
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
