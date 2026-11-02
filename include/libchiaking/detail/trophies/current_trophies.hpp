/// @file
/// @brief Declares CurrentTrophies, the trophies of the game being played as
/// PSN has them now.
#pragma once

#include "libchiaking/detail/trophies/title_trophies.hpp"
#include "libchiaking/detail/trophies/trophy_title.hpp"

namespace libchiaking::trophies {

/// @brief The trophy set of the game being played and its trophies, as PSN
/// has them at the time of asking. Plain data.
///
/// @see Trophies::current_trophies
struct CurrentTrophies {
  /// @brief The game's trophy set, with the account's progress in it.
  TrophyTitle title;
  /// @brief Its groups and trophies, each merged with what the account has
  /// earned.
  TitleTrophies trophies;
};

}  // namespace libchiaking::trophies
