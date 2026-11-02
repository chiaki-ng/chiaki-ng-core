/// @file
/// @brief Declares SequenceNumber, a wrapping counter ordered by serial
/// number arithmetic (RFC 1982), such as the 16-bit frame index of a
/// CloudStream media stream.
#pragma once

#include <compare>
#include <concepts>
#include <limits>

namespace libchiaking::numeric {

/// @brief A sequence number that wraps around at its type's width and
/// compares by which of two numbers is ahead within half the range.
///
/// `a < b` means `b` is less than half the range ahead of `a`, counting
/// forward with wrap-around, so 65535 is before 0 for 16-bit numbers. Two
/// numbers exactly half the range apart are unordered. The order is
/// therefore not transitive over the whole range and must only compare
/// numbers that are known to be close.
///
/// A plain value; copy freely.
///
/// @tparam Value The unsigned integer type of the counter, such as
///     `std::uint16_t`.
template <std::unsigned_integral Value>
class SequenceNumber {
 public:
  /// @brief Half the number space; numbers this far apart are unordered.
  static constexpr Value half_range =
      Value{1} << (std::numeric_limits<Value>::digits - 1);

  /// @brief Makes sequence number zero.
  constexpr SequenceNumber() noexcept = default;

  /// @brief Makes the sequence number with the given wire value.
  ///
  /// @param value The raw counter value.
  constexpr explicit SequenceNumber(Value value) noexcept : value_(value) {}

  /// @brief Returns the raw counter value, as sent on the wire.
  ///
  /// @return The value.
  constexpr auto value() const noexcept -> Value { return value_; }

  /// @brief Returns the number after this one, wrapping to zero after the
  /// maximum.
  ///
  /// @return This number advanced by one.
  constexpr auto next() const noexcept -> SequenceNumber {
    return advanced_by(1);
  }

  /// @brief Returns the number a given count ahead, wrapping around.
  ///
  /// @param steps How far to advance.
  /// @return This number advanced by `steps`, modulo the range.
  constexpr auto advanced_by(Value steps) const noexcept -> SequenceNumber {
    return SequenceNumber{static_cast<Value>(value_ + steps)};
  }

  /// @brief Counts forward from this number to another, wrapping around.
  ///
  /// @param later The number to count to.
  /// @return How many steps forward `later` is; a number "behind" this one
  ///     comes out as a large distance, never negative.
  constexpr auto distance_to(SequenceNumber later) const noexcept -> Value {
    return static_cast<Value>(later.value_ - value_);
  }

  /// @brief Tells whether this number is ahead of another by less than half
  /// the range.
  ///
  /// @param other The number to compare with.
  /// @return True when this number is ahead; false when equal, behind or
  ///     exactly half the range away.
  constexpr auto is_newer_than(SequenceNumber other) const noexcept -> bool {
    return *this != other && other.distance_to(*this) < half_range;
  }

  /// @brief Compares raw values for equality.
  ///
  /// @return True when both carry the same value.
  friend constexpr auto operator==(SequenceNumber, SequenceNumber) noexcept
      -> bool = default;

  /// @brief Orders two numbers by serial number arithmetic.
  ///
  /// @param left One number.
  /// @param right The other number.
  /// @return `equivalent` when equal, `greater` when `left` is newer,
  ///     `less` when `right` is newer, and `unordered` when they are
  ///     exactly half the range apart.
  friend constexpr auto operator<=>(SequenceNumber left,
                                    SequenceNumber right) noexcept
      -> std::partial_ordering {
    if (left == right) return std::partial_ordering::equivalent;
    if (left.is_newer_than(right)) return std::partial_ordering::greater;
    if (right.is_newer_than(left)) return std::partial_ordering::less;
    return std::partial_ordering::unordered;
  }

 private:
  /// @brief The raw counter value.
  Value value_{};
};

}  // namespace libchiaking::numeric
