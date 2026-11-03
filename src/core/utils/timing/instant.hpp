/// @file
/// @brief Declares Instant, a point in time read from an ISO 8601 timestamp
/// that names its zone.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

namespace libchiaking::timing {

/// @brief A point in time from an ISO 8601 timestamp that names its zone, as
/// PS's REST services write them (`2024-05-01T12:00:00.000Z`).
///
/// Held in UTC with millisecond precision. A timestamp without `Z` or a
/// numeric offset is refused rather than guessed to be local or UTC. A plain
/// value; copy freely.
class Instant {
 public:
  /// @brief A UTC wall-clock time in milliseconds.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief Wraps a time.
  ///
  /// @param time The UTC time.
  constexpr explicit Instant(Time time) noexcept : time_(time) {}

  /// @brief Reads an ISO 8601 timestamp, letters in either case.
  ///
  /// @param text The timestamp, unquoted, ending in `Z` or a `+hh:mm` or
  ///     `-hh:mm` offset.
  /// @return The instant, or nothing when the zone is missing or glaze
  ///     cannot read the timestamp.
  static auto from_text(std::string_view text) -> std::optional<Instant>;

  /// @brief Returns the time.
  ///
  /// @return The UTC time in milliseconds.
  constexpr auto time() const noexcept -> Time { return time_; }

  /// @brief Compares two instants.
  ///
  /// @return True when both are the same millisecond.
  friend constexpr auto operator==(Instant, Instant) noexcept -> bool = default;

 private:
  /// @brief The length of a numeric zone offset, `+hh:mm`.
  static constexpr std::size_t offset_width = 6;

  /// @brief Tells whether a timestamp ends with a zone designator.
  ///
  /// @param text The timestamp.
  /// @return True when it ends in `Z`, `z`, or a sign, two characters, a
  ///     colon and two characters.
  static constexpr auto names_its_zone(std::string_view text) noexcept -> bool {
    if (text.ends_with('Z') || text.ends_with('z')) return true;
    if (text.size() < offset_width) return false;
    const auto offset = text.substr(text.size() - offset_width);
    return (offset[0] == '+' || offset[0] == '-') && offset[3] == ':';
  }

  /// @brief The time, in UTC.
  Time time_;
};

}  // namespace libchiaking::timing
