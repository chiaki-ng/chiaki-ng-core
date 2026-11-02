/// @file
/// @brief Declares TrophyTitle, one game in an account's trophy list, with
/// TrophySet and TrophyPlatform, which say which trophy system and consoles
/// it belongs to.
#pragma once

#include "libchiaking/detail/trophies/ids.hpp"
#include "libchiaking/detail/trophies/trophy_counts.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::trophies {

/// @brief Which of PSN's two trophy systems a set belongs to.
///
/// PSN names it `npServiceName`; every request about a set's groups and
/// trophies states it.
enum class TrophySet : std::uint8_t {
  /// @brief The system of PS3, PS4 and PS Vita titles (`trophy`).
  classic,
  /// @brief The system of PS5 titles (`trophy2`).
  ps5,
};

/// @brief A console a trophy set is earned on.
enum class TrophyPlatform : std::uint8_t {
  /// @brief PlayStation 3.
  ps3,
  /// @brief PlayStation 4.
  ps4,
  /// @brief PlayStation 5.
  ps5,
  /// @brief PS Vita.
  ps_vita,
  /// @brief A PlayStation title on PC.
  pc,
};

/// @brief One game in an account's trophy list, with how far the account has
/// got. Plain data.
///
/// Pass it to Trophies::trophies_of to read the game's groups and trophies.
///
/// @see Trophies::titles_of
struct TrophyTitle {
  /// @brief The id of the title's trophy set.
  NpCommunicationId id;
  /// @brief The trophy system the set belongs to; nothing when PSN named one
  /// this library does not know.
  std::optional<TrophySet> set;
  /// @brief The title's name.
  std::string name;
  /// @brief A short description of the set; often empty.
  std::string detail;
  /// @brief Address of the set's icon.
  std::string icon_url;
  /// @brief The consoles the set is earned on, in PSN's order; empty when PSN
  /// named none this library knows.
  std::vector<TrophyPlatform> platforms;
  /// @brief The version of the set, such as `01.00`.
  std::string set_version;
  /// @brief Whether the set has groups beyond the base game, such as
  /// add-ons.
  bool has_groups{};
  /// @brief How many groups the set has, the base game's included.
  std::uint32_t group_count{};
  /// @brief The trophies the set holds, by grade.
  TrophyCounts defined;
  /// @brief The trophies the account has earned in the set, by grade.
  TrophyCounts earned;
  /// @brief How much of the set the account has earned, in percent, as PSN
  /// weighs it.
  std::uint32_t progress{};
  /// @brief Whether the account hid the title from its public trophy list.
  bool hidden{};
  /// @brief When the account's progress in the set last changed; nothing
  /// when PSN gave no readable time.
  std::optional<Time> last_updated;

  /// @brief Compares every field.
  friend auto operator==(const TrophyTitle&, const TrophyTitle&)
      -> bool = default;
};

}  // namespace libchiaking::trophies
