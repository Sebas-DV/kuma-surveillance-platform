#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace domain {

enum class ErrorKind {
    InvalidArgument,
    InvalidState,
    NotFound,
    AlreadyExists,
    Unauthenticated,
    AccountLocked,
    Forbidden,
    Unreachable,
    Timeout,
    Busy,
    Unsupported,
    Protocol,
    Internal
};

[[nodiscard]] std::string_view toString(ErrorKind kind) noexcept;

struct Error {
    ErrorKind kind{ErrorKind::Internal};
    std::string message;
    std::int64_t nativeCode{0};
    bool retryable{false};
};

template <typename T>
class [[nodiscard]] Result {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool hasValue() const noexcept { return data_.index() == 0; }

    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & { return std::get<0>(data_); }

    [[nodiscard]] const T& value() const& { return std::get<0>(data_); }

    [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(data_); }

private:
    template <std::size_t Index, typename Argument>
    Result(std::in_place_index_t<Index> tag, Argument&& argument)
        : data_(tag, std::forward<Argument>(argument)) {}

    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    [[nodiscard]] static Result success() { return Result(); }

    [[nodiscard]] static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }

    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] const Error& error() const& { return *error_; }

private:
    Result() = default;

    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

}
