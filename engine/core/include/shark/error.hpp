#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace shark {

// Names returned below are the stable discriminator; messages are for people.
enum class ErrorCode {
    invalid_argument,
    invalid_configuration,
    invalid_id,
    invalid_path,
    path_outside_root,
    io_failure,
    entropy_unavailable,
    capability_unavailable,
    conflict,
    execution_failed,
};

struct ErrorContext {
    std::string operation{};
    std::string request_id{};
    std::string path{};
    std::string entity_id{};
    std::string asset_id{};
};

struct Error {
    ErrorCode code{ErrorCode::execution_failed};
    std::string message{};
    ErrorContext context{};
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;
[[nodiscard]] std::string_view error_category_name(ErrorCode code) noexcept;

// Both alternatives own their storage. Inspect has_value() before accessing an
// alternative. A wrong-alternative access is a programmer error and throws
// std::bad_variant_access in Debug and Release; it is not an expected failure.
template <typename T> class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>);
    static_assert(std::is_object_v<T> && !std::is_array_v<T>);

  public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0;
    }
    explicit operator bool() const noexcept {
        return has_value();
    }
    [[nodiscard]] T& value() & {
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        return std::get<0>(std::move(storage_));
    }
    [[nodiscard]] const T&& value() const&& {
        return std::get<0>(std::move(storage_));
    }
    [[nodiscard]] Error& error() & {
        return std::get<1>(storage_);
    }
    [[nodiscard]] const Error& error() const& {
        return std::get<1>(storage_);
    }
    [[nodiscard]] Error&& error() && {
        return std::get<1>(std::move(storage_));
    }
    [[nodiscard]] const Error&& error() const&& {
        return std::get<1>(std::move(storage_));
    }

  private:
    std::variant<T, Error> storage_;
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() noexcept = default;
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0;
    }
    explicit operator bool() const noexcept {
        return has_value();
    }
    void value() const {
        static_cast<void>(std::get<0>(storage_));
    }
    [[nodiscard]] Error& error() & {
        return std::get<1>(storage_);
    }
    [[nodiscard]] const Error& error() const& {
        return std::get<1>(storage_);
    }
    [[nodiscard]] Error&& error() && {
        return std::get<1>(std::move(storage_));
    }
    [[nodiscard]] const Error&& error() const&& {
        return std::get<1>(std::move(storage_));
    }

  private:
    std::variant<std::monostate, Error> storage_;
};

} // namespace shark
