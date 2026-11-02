/// @file
/// @brief Declares Freshness, the rule for when a kept trophy answer may be
/// reused instead of asking PSN again.
#pragma once

#include <libchiaking/detail/trophies/title_trophies.hpp>
#include <libchiaking/detail/trophies/trophy_counts.hpp>
#include <libchiaking/detail/trophies/trophy_title.hpp>

#include <chrono>

namespace libchiaking::trophies {

/// @brief Decides whether a kept answer is still good. A plain value; pure.
class Freshness {
 public:
  /// @brief Builds the rule.
  ///
  /// @param lifetime How long an answer is reused by age alone.
  explicit constexpr Freshness(std::chrono::milliseconds lifetime) noexcept
      : lifetime_(lifetime) {}

  /// @brief Whether an answer written at one time is young enough.
  ///
  /// @param written_at When the answer was kept.
  /// @param now The current wall-clock time.
  /// @return True from written_at until the lifetime has passed; false for
  ///     a time before written_at, so a clock stepped back revives nothing.
  constexpr auto fresh(Time written_at, Time now) const noexcept -> bool {
    return now >= written_at && now - written_at < lifetime_;
  }

  /// @brief Whether kept trophies still describe a title.
  ///
  /// @param kept The kept trophies.
  /// @param written_at When they were kept.
  /// @param title The title as just listed.
  /// @param now The current wall-clock time.
  /// @return When the title carries an update time, whether it equals the
  ///     one the trophies were read for; otherwise whether they are fresh().
  auto still_matches(const TitleTrophies& kept, Time written_at,
                     const TrophyTitle& title, Time now) const noexcept
      -> bool {
    if (title.last_updated) return kept.last_updated == title.last_updated;
    return fresh(written_at, now);
  }

 private:
  /// @brief How long an answer is reused by age alone.
  std::chrono::milliseconds lifetime_;
};

}  // namespace libchiaking::trophies
