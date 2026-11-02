/// @file
/// @brief Declares TitleSets, the trophy sets PSN files one console release
/// of a game under.
#pragma once

#include "libchiaking/detail/trophies/ids.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"

#include <vector>

namespace libchiaking::trophies {

/// @brief The trophy sets PSN files one console release of a game under.
/// Plain data.
///
/// This is what links a game the account played, as PSN's play history
/// names it, to the titles Trophies::titles_of lists: each set id here is
/// the TrophyTitle::id of one of them.
///
/// @see Trophies::titles_for
struct TitleSets {
  /// @brief The release asked about.
  NpTitleId title;
  /// @brief Its trophy sets in PSN's order, each listed once; empty when PSN
  /// knows none for it, as for a release without trophies or one the account
  /// has no trophy record of yet.
  std::vector<NpCommunicationId> sets;

  /// @brief Compares the release and its sets.
  friend auto operator==(const TitleSets&, const TitleSets&) -> bool = default;
};

}  // namespace libchiaking::trophies
