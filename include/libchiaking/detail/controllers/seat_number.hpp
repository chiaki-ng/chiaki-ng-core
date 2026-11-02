/// @file
/// @brief Declares SeatNumber, the player seat a controller's state and
/// feedback belong to.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::controllers {

/// @brief Identifies one of a session's four player seats.
///
/// Seat 0 is the first player, the one the session was started for. Seats 1 to
/// 3 are added seats, filled by further people playing on the same session. A
/// SeatNumber is always in range.
class SeatNumber {
 public:
  /// @brief Number of seats in a session.
  static constexpr std::size_t count = 4;

  /// @brief Constructs the first player's seat, seat 0.
  constexpr SeatNumber() noexcept = default;

  /// @brief Makes a seat number from an index, checking the range.
  ///
  /// @param index Zero-based seat index.
  /// @return The seat, or nothing when index is not below count.
  [[nodiscard]] static constexpr auto create(std::size_t index) noexcept
      -> std::optional<SeatNumber> {
    if (index >= count) return std::nullopt;
    return SeatNumber{static_cast<std::uint8_t>(index)};
  }

  /// @brief Makes a seat number from a byte received from a console, keeping
  /// only its two low bits.
  ///
  /// @param wire_index The byte as received.
  /// @return A seat that is always in range.
  static constexpr auto masked(std::uint8_t wire_index) noexcept -> SeatNumber {
    return SeatNumber{static_cast<std::uint8_t>(wire_index & (count - 1))};
  }

  /// @brief Returns the first player's seat, seat 0.
  ///
  /// @return Seat 0.
  static constexpr auto first() noexcept -> SeatNumber { return SeatNumber{}; }

  /// @brief Zero-based index of the seat, 0 to 3.
  ///
  /// @return The index.
  constexpr auto index() const noexcept -> std::uint8_t { return index_; }
  /// @brief Reports whether this is seat 0, the first player.
  ///
  /// @return `true` for seat 0.
  constexpr auto is_first() const noexcept -> bool { return index_ == 0; }
  /// @brief Reports whether this is one of seats 1 to 3, which can be added
  /// and removed.
  ///
  /// @return `true` for seats 1 to 3.
  constexpr auto is_added() const noexcept -> bool { return index_ != 0; }

  /// @brief Compares seat indices.
  friend constexpr auto operator==(const SeatNumber&,
                                   const SeatNumber&) noexcept
      -> bool = default;
  /// @brief Orders seats by index.
  friend constexpr auto operator<=>(const SeatNumber&,
                                    const SeatNumber&) noexcept
      -> std::strong_ordering = default;

 private:
  constexpr explicit SeatNumber(std::uint8_t index) noexcept : index_(index) {}

  std::uint8_t index_{};
};

}  // namespace libchiaking::controllers
