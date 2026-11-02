/// @file
/// @brief Declares TrophyGroup, a group of trophies within a set, such as
/// the base game or one add-on.
#pragma once

#include "libchiaking/detail/trophies/ids.hpp"
#include "libchiaking/detail/trophies/trophy_counts.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::trophies {

/// @brief A group of trophies within a set, with how far the account has got
/// in it. Plain data.
///
/// A set without groups beyond the base game has exactly one, whose id is
/// TrophyGroupId::base_game.
///
/// @see TitleTrophies
struct TrophyGroup {
  /// @brief The group's id within the set.
  TrophyGroupId id;
  /// @brief The group's name: the add-on's, or the title's for the base
  /// game.
  std::string name;
  /// @brief A short description; often empty.
  std::string detail;
  /// @brief Address of the group's icon.
  std::string icon_url;
  /// @brief The trophies the group holds, by grade.
  TrophyCounts defined;
  /// @brief The trophies the account has earned in the group, by grade.
  TrophyCounts earned;
  /// @brief How much of the group the account has earned, in percent; 0
  /// when PSN did not report the group's progress.
  std::uint32_t progress{};
  /// @brief When the account's progress in the group last changed; nothing
  /// when PSN gave no readable time.
  std::optional<Time> last_updated;

  /// @brief Compares every field.
  friend auto operator==(const TrophyGroup&, const TrophyGroup&)
      -> bool = default;
};

}  // namespace libchiaking::trophies
