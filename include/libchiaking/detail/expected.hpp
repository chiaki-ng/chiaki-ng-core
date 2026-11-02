/// @file
/// @brief Declares Expected, the value-or-error return type used across the
/// library.
#pragma once

#include <expected>
#include <utility>

namespace libchiaking {

/// @brief The error wrapper that converts to the error state of any
/// `Expected<T, E>`; it is `std::unexpected`.
/// @tparam E The error type.
/// @see unexpected
template <class E>
using Unexpected = std::unexpected<E>;

/// @brief Marks a value as an error for returning from a function that returns
/// `Expected<T, E>`.
///
/// Written as `return unexpected (Error::busy);`.
///
/// @tparam E The error type, deduced from the argument.
/// @param error The error to carry.
/// @return The error wrapped as `std::unexpected` for conversion to an
///     Expected.
template <class E>
[[nodiscard]] constexpr auto unexpected(E error) -> Unexpected<E> {
  return Unexpected<E>{std::move(error)};
}

/// @brief Holds either the value a call produced or the error that stopped it;
/// it is `std::expected`.
///
/// Test it with has_value() or in a condition, then read value() or error().
/// The library reports every expected failure this way and does not throw for
/// them. `Expected<void, E>` is the form for calls that produce no value, and
/// its success is written `return {};`.
///
/// @tparam T The value type on success.
/// @tparam E The error type on failure.
/// @warning value() on an error throws `std::bad_expected_access<E>`, and
///     error() on a value is undefined behaviour.
template <class T, class E>
using Expected = std::expected<T, E>;

}  // namespace libchiaking
