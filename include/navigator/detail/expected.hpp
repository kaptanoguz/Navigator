// navigator/detail/expected.hpp
//
// Purpose: minimal std::expected stand-in, used by navigator::Result<T> when the standard library
//          does not provide std::expected (C++20 mode, or Clang 18 + libstdc++ 13 in C++23 mode).
// Owner:   contracts.
// Thread-safety: value type; same rules as any std:: value type (no internal synchronization).
//
// Only the subset that Navigator code may use in BOTH modes is provided:
//   has_value(), explicit operator bool, operator*, operator->, value(), error(), value_or(),
//   construction from a value, from Unexpected<E>, and default construction (Expected<void, E>).
// Monadic members (and_then, transform, ...) and comparisons are intentionally absent; do not use
// them on navigator::Result even when std::expected is active.
//
// Misuse (value() on an error, error() on a value) is a programming bug. std::expected throws
// bad_expected_access from value(); this fallback calls std::abort(). Always check first.
#pragma once

#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace navigator::detail {

template <class E> class Unexpected {
public:
    constexpr explicit Unexpected(const E& e) : error_(e) {}
    constexpr explicit Unexpected(E&& e) : error_(std::move(e)) {}

    constexpr const E& error() const& noexcept { return error_; }
    constexpr E& error() & noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }

private:
    E error_;
};

template <class E> Unexpected(E) -> Unexpected<E>;

template <class T> struct IsUnexpected : std::false_type {};
template <class E> struct IsUnexpected<Unexpected<E>> : std::true_type {};

template <class T, class E> class Expected {
    static constexpr std::size_t kValue = 0;
    static constexpr std::size_t kError = 1;

public:
    using value_type = T;
    using error_type = E;

    constexpr Expected()
        requires std::is_default_constructible_v<T>
        : storage_(std::in_place_index<kValue>) {}

    template <class U = T>
        requires(!std::is_same_v<std::remove_cvref_t<U>, Expected> &&
                 !IsUnexpected<std::remove_cvref_t<U>>::value && std::is_constructible_v<T, U &&>)
    constexpr Expected(U&& v) // NOLINT(google-explicit-constructor): mirrors std::expected
        : storage_(std::in_place_index<kValue>, std::forward<U>(v)) {}

    template <class G>
        requires std::is_constructible_v<E, const G&>
    constexpr Expected(const Unexpected<G>& u) // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<kError>, u.error()) {}

    template <class G>
        requires std::is_constructible_v<E, G&&>
    constexpr Expected(Unexpected<G>&& u) // NOLINT(google-explicit-constructor)
        : storage_(std::in_place_index<kError>, std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return storage_.index() == kValue; }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr T& value() & { return check(), std::get<kValue>(storage_); }
    constexpr const T& value() const& { return check(), std::get<kValue>(storage_); }
    constexpr T&& value() && { return check(), std::get<kValue>(std::move(storage_)); }

    constexpr T& operator*() & noexcept { return *std::get_if<kValue>(&storage_); }
    constexpr const T& operator*() const& noexcept { return *std::get_if<kValue>(&storage_); }
    constexpr T&& operator*() && noexcept { return std::move(*std::get_if<kValue>(&storage_)); }
    constexpr T* operator->() noexcept { return std::get_if<kValue>(&storage_); }
    constexpr const T* operator->() const noexcept { return std::get_if<kValue>(&storage_); }

    constexpr E& error() & noexcept { return *std::get_if<kError>(&storage_); }
    constexpr const E& error() const& noexcept { return *std::get_if<kError>(&storage_); }
    constexpr E&& error() && noexcept { return std::move(*std::get_if<kError>(&storage_)); }

    template <class U> constexpr T value_or(U&& fallback) const& {
        return has_value() ? **this : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U> constexpr T value_or(U&& fallback) && {
        return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    constexpr void check() const {
        if (!has_value()) {
            std::abort();
        }
    }

    std::variant<T, E> storage_;
};

template <class E> class Expected<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Expected() noexcept = default;

    template <class G>
        requires std::is_constructible_v<E, const G&>
    constexpr Expected(const Unexpected<G>& u) // NOLINT(google-explicit-constructor)
        : error_(u.error()) {}

    template <class G>
        requires std::is_constructible_v<E, G&&>
    constexpr Expected(Unexpected<G>&& u) // NOLINT(google-explicit-constructor)
        : error_(std::move(u).error()) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return !error_.has_value(); }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    constexpr void value() const {
        if (!has_value()) {
            std::abort();
        }
    }
    constexpr void operator*() const noexcept {}

    // Precondition: !has_value() (same as std::expected::error()).
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    constexpr E& error() & noexcept { return *error_; }
    constexpr const E& error() const& noexcept { return *error_; }
    constexpr E&& error() && noexcept { return std::move(*error_); }
    // NOLINTEND(bugprone-unchecked-optional-access)

private:
    std::optional<E> error_;
};

} // namespace navigator::detail
