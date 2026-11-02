/// @file
/// @brief Declares TitleTrophies, a title's trophy groups and trophies with
/// the account's progress in each.
#pragma once

#include "libchiaking/detail/trophies/ids.hpp"
#include "libchiaking/detail/trophies/trophy.hpp"
#include "libchiaking/detail/trophies/trophy_group.hpp"

#include <optional>
#include <vector>

namespace libchiaking::trophies {

/// @brief Everything about one title's trophy set: its groups and its
/// trophies, each with the account's progress. Plain data.
///
/// @see Trophies::trophies_of
struct TitleTrophies {
  /// @brief The set these trophies belong to.
  NpCommunicationId title;
  /// @brief When the account's progress in the set last changed, as the
  /// TrophyTitle the trophies were read for said; nothing when it gave none.
  std::optional<Time> last_updated;
  /// @brief The groups in PSN's order, the base game first. Never empty.
  std::vector<TrophyGroup> groups;
  /// @brief Every trophy of the set, in PSN's order, which is by
  /// TrophyId.
  std::vector<Trophy> trophies;

  /// @brief Compares every field.
  friend auto operator==(const TitleTrophies&, const TitleTrophies&)
      -> bool = default;
};

}  // namespace libchiaking::trophies
