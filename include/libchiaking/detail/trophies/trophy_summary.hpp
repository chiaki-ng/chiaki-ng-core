/// @file
/// @brief Declares TrophySummary, an account's trophy level and the trophies
/// it has earned across every title.
#pragma once

#include "libchiaking/detail/trophies/trophy_counts.hpp"

#include <cstdint>

namespace libchiaking::trophies {

/// @brief An account's trophy level and everything it has earned, as PSN
/// sums it up. Plain data.
///
/// @see Trophies::summary_of
struct TrophySummary {
  /// @brief The trophy level, from 1.
  std::uint32_t level{};
  /// @brief The tier the level falls in, from 1 to 10; it decides the colour
  /// of the level badge.
  std::uint32_t tier{};
  /// @brief How far the account is towards the next level, in percent.
  std::uint32_t progress{};
  /// @brief The trophies earned, by grade.
  TrophyCounts earned;

  /// @brief Compares every field.
  friend auto operator==(const TrophySummary&, const TrophySummary&)
      -> bool = default;
};

}  // namespace libchiaking::trophies
