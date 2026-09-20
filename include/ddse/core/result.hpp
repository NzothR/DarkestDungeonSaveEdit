#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace ddse::core {

template <typename T, typename E>
class Result {
public:
    [[nodiscard]] static Result success(T value) { return Result{std::move(value)}; }
    [[nodiscard]] static Result failure(E error) { return Result{FailureTag{}, std::move(error)}; }

    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }
    [[nodiscard]] E& error() & { return std::get<E>(storage_); }
    [[nodiscard]] const E& error() const& { return std::get<E>(storage_); }

private:
    struct FailureTag {};
    explicit Result(T value) : storage_(std::in_place_type<T>, std::move(value)) {}
    Result(FailureTag, E error) : storage_(std::in_place_type<E>, std::move(error)) {}
    std::variant<T, E> storage_;
};

template <typename E>
class Result<void, E> {
public:
    [[nodiscard]] static Result success() { return Result{std::monostate{}}; }
    [[nodiscard]] static Result failure(E error) { return Result{std::move(error)}; }
    [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
    explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] E& error() & { return std::get<E>(storage_); }
    [[nodiscard]] const E& error() const& { return std::get<E>(storage_); }

private:
    explicit Result(std::monostate value) : storage_(value) {}
    explicit Result(E error) : storage_(std::move(error)) {}
    std::variant<std::monostate, E> storage_;
};

} // namespace ddse::core
