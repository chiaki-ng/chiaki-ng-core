/// @file
/// @brief Declares Trophy, one trophy of a set with whether the account has
/// earned it, and Rarity, how rare PSN says it is.
#pragma once

#include "libchiaking/detail/trophies/ids.hpp"
#include "libchiaking/detail/trophies/trophy_counts.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::trophies {

/// @brief How rare a trophy is among the players of its title, as PSN
/// buckets it.
enum class Rarity : std::uint8_t {
  /// @brief Earned by very few players.
  ultra_rare,
  /// @brief Earned by few players.
  very_rare,
  /// @brief Earned by some players.
  rare,
  /// @brief Earned by many players.
  common,
};

/// @brief How far the account has got towards a trophy that counts something,
/// such as a number of wins. Plain data.
struct TrophyProgress {
  /// @brief The count reached so far.
  std::uint64_t value{};
  /// @brief The count reached, in percent of the target.
  std::uint32_t percent{};
  /// @brief When the count last changed; nothing when PSN gave no readable
  /// time.
  std::optional<Time> updated_at;

  /// @brief Compares every field.
  friend auto operator==(const TrophyProgress&, const TrophyProgress&)
      -> bool = default;
};

/// @brief One trophy of a set: what it is, from the set's definition, merged
/// with whether and when the account earned it. Plain data.
///
/// A trophy PSN reported no earned state for reads as not earned, with no
/// rarity and no progress.
///
/// @see TitleTrophies
struct Trophy {
  /// @brief The trophy's number within its set.
  TrophyId id;
  /// @brief The group the trophy belongs to.
  TrophyGroupId group;
  /// @brief The trophy's grade.
  Grade grade{};
  /// @brief The trophy's name.
  std::string name;
  /// @brief What earns the trophy.
  std::string detail;
  /// @brief Address of the trophy's icon.
  std::string icon_url;
  /// @brief Whether the set hides the trophy's name and description until it
  /// is earned. The library still returns them; showing them is the
  /// caller's choice.
  bool hidden{};
  /// @brief The count a counting trophy needs; nothing for a trophy that
  /// counts nothing.
  std::optional<std::uint64_t> progress_target;
  /// @brief Whether the account has earned the trophy.
  bool earned{};
  /// @brief When the account earned it; nothing when not earned or PSN gave
  /// no readable time.
  std::optional<Time> earned_at;
  /// @brief How rare the trophy is; nothing when PSN did not say.
  std::optional<Rarity> rarity;
  /// @brief The share of the title's players who earned it, in percent;
  /// nothing when PSN did not say.
  std::optional<double> earned_rate;
  /// @brief How far the account has got towards a counting trophy; nothing
  /// for one that counts nothing or that PSN reported no count for.
  std::optional<TrophyProgress> progress;

  /// @brief Compares every field.
  friend auto operator==(const Trophy&, const Trophy&) -> bool = default;
};

}  // namespace libchiaking::trophies
