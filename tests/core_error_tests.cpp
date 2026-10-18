#include <shark/error.hpp>
#include <shark/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

class ShortWriteBuffer final : public std::streambuf {
  protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        return count - 1;
    }
};

class FailingFlushBuffer final : public std::stringbuf {
  protected:
    int sync() override {
        return -1;
    }
};

class ThrowingWriteBuffer final : public std::streambuf {
  protected:
    std::streamsize xsputn(const char*, std::streamsize) override {
        throw std::runtime_error("injected stream buffer failure");
    }
};

class ThrowingFlushBuffer final : public std::stringbuf {
  protected:
    int sync() override {
        throw std::runtime_error("injected flush failure");
    }
};

shark::Error example_error() {
    return {shark::ErrorCode::invalid_configuration,
            "A candidate was rejected",
            {.operation = "configuration.validate", .request_id = "request-17"}};
}

} // namespace

TEST_CASE("Results own values and errors without borrowed context", "[core][diagnostics]") {
    auto make_error = [] {
        std::string message = "Candidate rejected";
        std::string request = "request-17";
        return shark::Result<int>{
            shark::Error{shark::ErrorCode::conflict,
                         message,
                         {.operation = "scene.commit", .request_id = request}}};
    };
    auto failed = make_error();
    REQUIRE_FALSE(failed.has_value());
    REQUIRE_FALSE(static_cast<bool>(failed));
    REQUIRE(failed.error().code == shark::ErrorCode::conflict);
    REQUIRE(failed.error().message == "Candidate rejected");
    REQUIRE(failed.error().context.operation == "scene.commit");
    REQUIRE(failed.error().context.request_id == "request-17");

    const shark::Result<std::string> success{std::string{"owned"}};
    REQUIRE(success.has_value());
    REQUIRE(static_cast<bool>(success));
    REQUIRE(success.value() == "owned");
    auto copied = failed;
    copied.error().message = "Changed copy";
    REQUIRE(failed.error().message == "Candidate rejected");
    const auto& constant = failed;
    REQUIRE(constant.error().context.request_id == "request-17");
    auto moved_error = std::move(failed).error();
    REQUIRE(moved_error.message == "Candidate rejected");
}

TEST_CASE("Results support move-only values and checked alternative access",
          "[core][diagnostics]") {
    static_assert(!std::is_copy_constructible_v<shark::Result<std::unique_ptr<int>>>);
    shark::Result<std::unique_ptr<int>> original{std::make_unique<int>(42)};
    auto moved = std::move(original);
    REQUIRE(*moved.value() == 42);
    auto owned = std::move(moved).value();
    REQUIRE(*owned == 42);
    shark::Result<int> failed{example_error()};
    REQUIRE_THROWS_AS(failed.value(), std::bad_variant_access);
    shark::Result<int> success{7};
    REQUIRE_THROWS_AS(success.error(), std::bad_variant_access);
}

TEST_CASE("Void results preserve success and error semantics", "[core][diagnostics]") {
    shark::Result<void> success;
    REQUIRE(success.has_value());
    REQUIRE_NOTHROW(success.value());
    REQUIRE_THROWS_AS(success.error(), std::bad_variant_access);
    shark::Result<void> failure{example_error()};
    REQUIRE_FALSE(failure.has_value());
    REQUIRE_THROWS_AS(failure.value(), std::bad_variant_access);
    const auto& constant = failure;
    REQUIRE(constant.error().context.operation == "configuration.validate");
    auto moved = std::move(failure).error();
    REQUIRE(moved.code == shark::ErrorCode::invalid_configuration);
}

TEST_CASE("Error codes retain stable machine names and distinct categories",
          "[core][diagnostics]") {
    struct Mapping {
        shark::ErrorCode code;
        std::string_view name;
        std::string_view category;
    };
    const std::array mappings{
        Mapping{shark::ErrorCode::invalid_argument, "invalid_argument", "validation"},
        Mapping{shark::ErrorCode::invalid_configuration, "invalid_configuration", "validation"},
        Mapping{shark::ErrorCode::invalid_id, "invalid_id", "validation"},
        Mapping{shark::ErrorCode::invalid_path, "invalid_path", "validation"},
        Mapping{shark::ErrorCode::path_outside_root, "path_outside_root", "validation"},
        Mapping{shark::ErrorCode::io_failure, "io_failure", "execution"},
        Mapping{shark::ErrorCode::entropy_unavailable, "entropy_unavailable", "unavailable"},
        Mapping{shark::ErrorCode::capability_unavailable, "capability_unavailable", "unavailable"},
        Mapping{shark::ErrorCode::conflict, "conflict", "conflict"},
        Mapping{shark::ErrorCode::execution_failed, "execution_failed", "execution"},
    };
    for (const auto& mapping : mappings) {
        INFO(mapping.name);
        REQUIRE(shark::error_code_name(mapping.code) == mapping.name);
        REQUIRE(shark::error_category_name(mapping.code) == mapping.category);
    }
}

TEST_CASE("Logger emits one escaped JSON line with owned correlation context",
          "[core][diagnostics]") {
    auto error = example_error();
    error.message = "quote\" slash\\ \n\t\b\f\r";
    error.message.push_back('\0');
    error.context.path = "content/海 🦈/world.json";
    error.context.entity_id = "entity-7";
    error.context.asset_id = "asset-9";
    std::ostringstream sink;
    shark::Logger logger{sink};
    const auto result = logger.write(shark::LogLevel::warning, error);
    REQUIRE(result.has_value());
    const std::string expected =
        R"({"level":"warning","code":"invalid_configuration","category":"validation","message":"quote\" slash\\ \u000a\u0009\u0008\u000c\u000d\u0000","operation":"configuration.validate","request_id":"request-17","path":"content/海 🦈/world.json","entity_id":"entity-7","asset_id":"asset-9"})"
        "\n";
    REQUIRE(sink.str() == expected);
}

TEST_CASE("Logger omits empty optional fields and never silently filters severity",
          "[core][diagnostics]") {
    const shark::Error error{shark::ErrorCode::execution_failed, "failure", {.operation = "test"}};
    const std::array levels{shark::LogLevel::debug, shark::LogLevel::info, shark::LogLevel::warning,
                            shark::LogLevel::error};
    const std::array<std::string_view, 4> names{"debug", "info", "warning", "error"};
    for (std::size_t index = 0; index < levels.size(); ++index) {
        std::ostringstream sink;
        shark::Logger logger{sink};
        REQUIRE(logger.write(levels[index], error).has_value());
        REQUIRE(sink.str() == "{\"level\":\"" + std::string(names[index]) +
                                  "\",\"code\":\"execution_failed\",\"category\":\"execution\","
                                  "\"message\":\"failure\",\"operation\":\"test\"}\n");
    }
}

TEST_CASE("Logger bounds every field before touching its sink", "[core][diagnostics]") {
    for (std::size_t index = 0; index < 6; ++index) {
        auto error = example_error();
        const std::array fields{
            &error.message,      &error.context.operation, &error.context.request_id,
            &error.context.path, &error.context.entity_id, &error.context.asset_id};
        const auto limit =
            index == 3 ? shark::Logger::max_path_bytes : shark::Logger::max_field_bytes;
        *fields[index] = std::string(limit + 1, 'x');
        std::ostringstream sink;
        shark::Logger logger{sink};
        const auto result = logger.write(shark::LogLevel::error, error);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == shark::ErrorCode::invalid_argument);
        REQUIRE(result.error().context.operation == "log.write");
        REQUIRE(sink.str().empty());
    }

    auto error = example_error();
    const std::array fields{
        &error.message,      &error.context.operation, &error.context.request_id,
        &error.context.path, &error.context.entity_id, &error.context.asset_id};
    for (auto* field : fields) {
        *field = std::string(shark::Logger::max_field_bytes, '\0');
    }
    error.context.path = std::string(shark::Logger::max_path_bytes, '\0');
    std::ostringstream sink;
    shark::Logger logger{sink};
    REQUIRE(logger.write(shark::LogLevel::error, error).has_value());
    REQUIRE(sink.str().size() < shark::Logger::max_record_bytes);
    REQUIRE(sink.str().find('\0') == std::string::npos);
}

TEST_CASE("Logger rejects malformed UTF-8 without producing malformed JSON",
          "[core][diagnostics]") {
    const std::array<std::string_view, 10> invalid{
        "\x80",         "\xff",         "\xc0\xaf",         "\xc1\xbf",
        "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80",
        "\xe2\x82",     "\xe2\x28\xa1",
    };
    for (const auto text : invalid) {
        auto error = example_error();
        error.context.path = text;
        std::ostringstream sink;
        shark::Logger logger{sink};
        const auto result = logger.write(shark::LogLevel::error, error);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == shark::ErrorCode::invalid_argument);
        REQUIRE(sink.str().empty());
    }
}

TEST_CASE("Logger reports write and flush failures without throwing or retrying",
          "[core][diagnostics]") {
    const auto error = example_error();
    auto check_failure = [&](std::ostream& sink) {
        shark::Logger logger{sink};
        const auto result = logger.write(shark::LogLevel::error, error);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == shark::ErrorCode::io_failure);
        REQUIRE(result.error().context.operation == "log.write");
    };
    SECTION("An already failed sink") {
        std::ostringstream sink;
        sink.setstate(std::ios::badbit);
        check_failure(sink);
        REQUIRE(sink.str().empty());
    }
    SECTION("A partial write") {
        ShortWriteBuffer buffer;
        std::ostream sink{&buffer};
        check_failure(sink);
    }
    SECTION("A failed flush") {
        FailingFlushBuffer buffer;
        std::ostream sink{&buffer};
        check_failure(sink);
    }
    SECTION("An exception during write") {
        ThrowingWriteBuffer buffer;
        std::ostream sink{&buffer};
        sink.exceptions(std::ios::badbit);
        check_failure(sink);
    }
    SECTION("An exception during flush") {
        ThrowingFlushBuffer buffer;
        std::ostream sink{&buffer};
        sink.exceptions(std::ios::badbit);
        check_failure(sink);
    }
}
