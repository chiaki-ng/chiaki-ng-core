/// @file
/// @brief Declares ReleaseDate, when an owned copy becomes playable.
#pragma once

#include "libchiaking/detail/catalog/wall_time.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::catalog {

/// @brief When an owned copy becomes playable: at once, at a set time, or at a
/// time not yet announced.
///
/// Whether a scheduled date has passed is decided against trusted time, never
/// the device clock, so changing the clock cannot unlock a preorder.
///
/// @see TrustedClock
/// @see LaunchRefusal::of_release
class ReleaseDate {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief How a release date stands at a given moment.
  enum class State : std::uint8_t {
    /// @brief The copy is playable.
    released,
    /// @brief The scheduled time is still ahead.
    not_released,
    /// @brief No release time has been announced.
    unannounced,
    /// @brief There is a scheduled time but no trusted time to compare it
    /// with.
    no_trusted_clock
  };

  /// @brief The time left until release, split for display.
  struct Countdown {
    /// @brief Whole days left.
    std::int64_t days{};
    /// @brief Hours left after the days, 0 to 23.
    int hours{};
    /// @brief Minutes left after the hours, 0 to 59.
    int minutes{};
    /// @brief Seconds left after the minutes, 0 to 59.
    int seconds{};

    /// @brief Compares every field.
    friend auto operator==(const Countdown&, const Countdown&)
        -> bool = default;
  };

  /// @brief Builds an unrestricted date: the copy is playable at once.
  constexpr ReleaseDate() noexcept = default;

  /// @brief Builds the date of a copy that is playable at once.
  ///
  /// @return A date that never blocks launch.
  static constexpr auto unrestricted() noexcept -> ReleaseDate {
    return ReleaseDate{};
  }
  /// @brief Builds the date of a copy whose release time has not been
  /// announced.
  ///
  /// @return A date with no known release time, which blocks launch.
  static constexpr auto unannounced() noexcept -> ReleaseDate {
    return ReleaseDate{Kind::unannounced, Time{}};
  }
  /// @brief Builds the date of a copy that becomes playable at a set time.
  ///
  /// @param time The release time.
  /// @return The scheduled date, or an unrestricted one when the time is not
  ///     after the epoch.
  static constexpr auto at(Time time) noexcept -> ReleaseDate {
    return time > Time{} ? ReleaseDate{Kind::scheduled, time} : ReleaseDate{};
  }

  /// @brief Reads a date and time as PS writes them, as RFC 3339.
  ///
  /// Fractional seconds of any length are cut to milliseconds. The date must
  /// exist in the calendar, and the T and Z designators may be in either
  /// case. Never blocks.
  ///
  /// @param ps_date Text of the form YYYY-MM-DDThh:mm:ss, with or without
  ///     fractional seconds, ending in Z or a UTC offset such as +09:00.
  /// @return The time, or nothing when the text is empty, names no zone,
  ///     writes the offset without its colon, or is not of that form.
  [[nodiscard]] static auto parse_time(std::string_view ps_date)
      -> std::optional<Time>;
  /// @brief Works out the release date an entitlement record states.
  ///
  /// The playability date is PS's time the purchased edition becomes
  /// playable and is preferred when it reads as a time; an early access
  /// edition can be playable well before its activation date.
  ///
  /// @param active_date The record's activation date, when it has one.
  /// @param preorder Whether the record is flagged as a preorder.
  /// @param playable_from The record's playability date, when it has one.
  /// @return A scheduled date when the playability date, or else the
  ///     activation date, reads as a time after the epoch; unannounced when
  ///     the activation date is present but unreadable, or absent on a
  ///     preorder; otherwise unrestricted.
  static auto of_record(
      std::optional<std::string_view> active_date, bool preorder,
      std::optional<std::string_view> playable_from = std::nullopt)
      -> ReleaseDate;

  /// @brief Whether the copy is playable at once.
  ///
  /// @return True for an unrestricted date; false otherwise.
  constexpr auto is_unrestricted() const noexcept -> bool {
    return kind_ == Kind::unrestricted;
  }
  /// @brief Whether the release time has not been announced.
  ///
  /// @return True for an unannounced date; false otherwise.
  constexpr auto is_unannounced() const noexcept -> bool {
    return kind_ == Kind::unannounced;
  }
  /// @brief The scheduled release time; empty for unrestricted and
  /// unannounced dates.
  ///
  /// @return The release time, or nothing unless the date is scheduled.
  constexpr auto time() const noexcept -> std::optional<Time> {
    return kind_ == Kind::scheduled ? std::optional<Time>{time_} : std::nullopt;
  }

  /// @brief Returns whichever of two dates lets the copy be played sooner.
  ///
  /// Unrestricted comes before any scheduled time, and any scheduled time
  /// comes before unannounced.
  ///
  /// @param other The date to compare with.
  /// @return This date or the other, whichever comes first; this date when they
  ///     tie.
  auto earliest(ReleaseDate other) const noexcept -> ReleaseDate;
  /// @brief Returns whichever of two dates lets the copy be played later; see
  /// ReleaseDate::earliest for the order.
  ///
  /// @param other The date to compare with.
  /// @return This date or the other, whichever comes last; this date when they
  /// tie.
  auto latest(ReleaseDate other) const noexcept -> ReleaseDate;
  /// @brief Says how the date stands at a given moment.
  ///
  /// @param trusted_now The trusted current time, when one is available.
  /// @return ReleaseDate::State::released for an unrestricted date or a
  ///     scheduled time that is not ahead of trusted_now;
  ///     ReleaseDate::State::no_trusted_clock for a scheduled date when
  ///     trusted_now is empty.
  auto state(std::optional<Time> trusted_now) const noexcept -> State;
  /// @brief Whether the copy may not be played at the given moment, which is
  /// every state but released.
  ///
  /// @param trusted_now The trusted current time, when one is available.
  /// @return True when the date is not in the released state, including when
  ///     there is no trusted clock; false otherwise.
  auto blocks_launch(std::optional<Time> trusted_now) const noexcept -> bool;
  /// @brief Works out the time left until release.
  ///
  /// @param trusted_now The trusted current time.
  /// @return The time left, rounded up to whole seconds, or nothing unless
  ///     the date is scheduled and still ahead.
  auto countdown(Time trusted_now) const noexcept -> std::optional<Countdown>;

  /// @brief Compares kind and time.
  friend constexpr auto operator==(ReleaseDate, ReleaseDate) noexcept
      -> bool = default;

 private:
  enum class Kind : std::uint8_t { unrestricted, scheduled, unannounced };

  constexpr ReleaseDate(Kind kind, Time time) noexcept
      : kind_(kind), time_(time) {}

  auto comes_before(ReleaseDate other) const noexcept -> bool;

  Kind kind_{Kind::unrestricted};
  Time time_{};
};

}  // namespace libchiaking::catalog
