/// @file
/// @brief Declares overflow-checked unsigned arithmetic for the sizes, counts
/// and offsets of the Takion and Senkusha protocols, where a wrapped result
/// would read or write the wrong bytes.
#pragma once

#include <concepts>
#include <limits>
#include <optional>

namespace libchiaking::numeric {

/// @brief Adds two unsigned values, refusing to wrap.
///
/// @tparam Value The unsigned integer type.
/// @param left One addend.
/// @param right The other addend.
/// @return The sum, or nothing when it exceeds the type's maximum.
template <std::unsigned_integral Value>
constexpr auto checked_add(Value left, Value right) noexcept
    -> std::optional<Value> {
  if (right > std::numeric_limits<Value>::max() - left) return std::nullopt;
  return static_cast<Value>(left + right);
}

/// @brief Subtracts one unsigned value from another, refusing to go below
/// zero.
///
/// @tparam Value The unsigned integer type.
/// @param left The minuend.
/// @param right The subtrahend.
/// @return The difference, or nothing when `right` exceeds `left`.
template <std::unsigned_integral Value>
constexpr auto checked_sub(Value left, Value right) noexcept
    -> std::optional<Value> {
  if (right > left) return std::nullopt;
  return static_cast<Value>(left - right);
}

/// @brief Subtracts one unsigned value from another, stopping at zero.
///
/// @tparam Value The unsigned integer type.
/// @param left The minuend.
/// @param right The subtrahend.
/// @return The difference, or zero when `right` is at least `left`.
template <std::unsigned_integral Value>
constexpr auto saturating_sub(Value left, Value right) noexcept -> Value {
  return left > right ? static_cast<Value>(left - right) : Value{};
}

/// @brief Multiplies two unsigned values, refusing to wrap.
///
/// @tparam Value The unsigned integer type.
/// @param left One factor.
/// @param right The other factor.
/// @return The product, or nothing when it exceeds the type's maximum.
template <std::unsigned_integral Value>
constexpr auto checked_mul(Value left, Value right) noexcept
    -> std::optional<Value> {
  if (left != 0 && right > std::numeric_limits<Value>::max() / left)
    return std::nullopt;
  return static_cast<Value>(left * right);
}

}  // namespace libchiaking::numeric
