/// @file
/// @brief Declares TitleAssembly, the rules that merge a trophy set's
/// definitions with the account's earned state, and EarnedState and
/// GroupProgress, the earned facts it merges.
#pragma once

#include <libchiaking/detail/trophies/ids.hpp>
#include <libchiaking/detail/trophies/title_trophies.hpp>
#include <libchiaking/detail/trophies/trophy.hpp>
#include <libchiaking/detail/trophies/trophy_counts.hpp>
#include <libchiaking/detail/trophies/trophy_group.hpp>
#include <libchiaking/detail/trophies/trophy_title.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::trophies {

/// @brief What PSN reports the account has done towards one trophy. Plain
/// data.
struct EarnedState {
  /// @brief The trophy it is about.
  TrophyId id;
  /// @brief Whether the account earned it.
  bool earned{};
  /// @brief When it was earned; nothing when PS gave no readable time.
  std::optional<Time> earned_at;
  /// @brief How rare it is; nothing when PS did not say.
  std::optional<Rarity> rarity;
  /// @brief The share of players who earned it, in percent.
  std::optional<double> earned_rate;
  /// @brief The count reached towards a counting trophy.
  std::optional<TrophyProgress> progress;
};

/// @brief What PSN reports the account has earned in one group. Plain data.
struct GroupProgress {
  /// @brief The group it is about.
  TrophyGroupId id;
  /// @brief The trophies earned in the group, by grade.
  TrophyCounts earned;
  /// @brief How much of the group is earned, in percent.
  std::uint32_t progress{};
  /// @brief When the group's progress last changed.
  std::optional<Time> last_updated;
};

/// @brief Builds a title's TitleTrophies from its definitions and the
/// account's earned facts. Pure; safe from any thread.
class TitleAssembly {
 public:
  /// @brief Merges a set's definitions with what the account earned.
  ///
  /// Each trophy takes the earned state of the same TrophyId; one PS
  /// reported nothing for stays not earned. A title without groups, or
  /// without group definitions, gets one base_group(). Otherwise each group
  /// takes the progress of the same TrophyGroupId, or, when there is no
  /// progress at all, earned counts tallied from its earned trophies.
  ///
  /// @param title The title the set belongs to.
  /// @param groups The set's group definitions; ignored for a title
  ///     without groups.
  /// @param progress The account's group progress; nothing when it could
  ///     not be read.
  /// @param definitions The set's trophy definitions, in PS's order.
  /// @param earned The account's earned state for the set's trophies.
  /// @return The merged trophies, or nothing when the set defines no
  ///     trophy.
  [[nodiscard]] static auto assembled(
      const TrophyTitle& title, std::vector<TrophyGroup> groups,
      const std::optional<std::vector<GroupProgress>>& progress,
      std::vector<Trophy> definitions, std::span<const EarnedState> earned)
      -> std::optional<TitleTrophies>;
  /// @brief The one group of a set without groups, built from its title.
  ///
  /// @param title The title.
  /// @return A TrophyGroupId::base_game group carrying the title's name,
  ///     icon, counts, progress and update time.
  static auto base_group(const TrophyTitle& title) -> TrophyGroup;

 private:
  /// @brief Gives each definition the earned state of its trophy.
  ///
  /// @param definitions The trophies to update.
  /// @param earned The earned states, matched by TrophyId.
  static void merge(std::vector<Trophy>& definitions,
                    std::span<const EarnedState> earned);
  /// @brief Gives each group the progress PS reported for it.
  ///
  /// @param groups The groups to update.
  /// @param progress The progress, matched by TrophyGroupId.
  static void merge(std::vector<TrophyGroup>& groups,
                    std::span<const GroupProgress> progress);
  /// @brief Counts each group's earned trophies by grade.
  ///
  /// @param groups The groups to update.
  /// @param trophies The merged trophies.
  static void tally(std::vector<TrophyGroup>& groups,
                    std::span<const Trophy> trophies);
};

}  // namespace libchiaking::trophies
