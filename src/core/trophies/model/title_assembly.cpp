#include "src/core/trophies/model/title_assembly.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace libchiaking::trophies {

auto TitleAssembly::assembled(
    const TrophyTitle& title, std::vector<TrophyGroup> groups,
    const std::optional<std::vector<GroupProgress>>& progress,
    std::vector<Trophy> definitions, std::span<const EarnedState> earned)
    -> std::optional<TitleTrophies> {
  if (definitions.empty()) return std::nullopt;
  merge(definitions, earned);
  if (!title.has_groups || groups.empty()) {
    groups.assign(1, base_group(title));
  } else if (progress) {
    merge(groups, *progress);
  } else {
    tally(groups, definitions);
  }
  return TitleTrophies{.title = title.id,
                       .last_updated = title.last_updated,
                       .groups = std::move(groups),
                       .trophies = std::move(definitions)};
}

auto TitleAssembly::base_group(const TrophyTitle& title) -> TrophyGroup {
  return TrophyGroup{.id = TrophyGroupId::base_game(),
                     .name = title.name,
                     .detail = {},
                     .icon_url = title.icon_url,
                     .defined = title.defined,
                     .earned = title.earned,
                     .progress = title.progress,
                     .last_updated = title.last_updated};
}

void TitleAssembly::merge(std::vector<Trophy>& definitions,
                          std::span<const EarnedState> earned) {
  std::map<TrophyId, const EarnedState*> by_id;
  for (const auto& state : earned) by_id.insert_or_assign(state.id, &state);
  for (auto& trophy : definitions) {
    const auto found = by_id.find(trophy.id);
    if (found == by_id.end()) continue;
    const auto& state = *found->second;
    trophy.earned = state.earned;
    trophy.earned_at = state.earned ? state.earned_at : std::nullopt;
    trophy.rarity = state.rarity;
    trophy.earned_rate = state.earned_rate;
    if (state.progress) trophy.progress = state.progress;
  }
}

void TitleAssembly::merge(std::vector<TrophyGroup>& groups,
                          std::span<const GroupProgress> progress) {
  for (auto& group : groups) {
    const auto found =
        std::ranges::find(progress, group.id, &GroupProgress::id);
    if (found == progress.end()) continue;
    group.earned = found->earned;
    group.progress = found->progress;
    group.last_updated = found->last_updated;
  }
}

void TitleAssembly::tally(std::vector<TrophyGroup>& groups,
                          std::span<const Trophy> trophies) {
  for (auto& group : groups) {
    TrophyCounts earned;
    for (const auto& trophy : trophies) {
      if (!trophy.earned || trophy.group != group.id) continue;
      switch (trophy.grade) {
        case Grade::bronze:
          ++earned.bronze;
          break;
        case Grade::silver:
          ++earned.silver;
          break;
        case Grade::gold:
          ++earned.gold;
          break;
        case Grade::platinum:
          ++earned.platinum;
          break;
      }
    }
    group.earned = earned;
  }
}

}  // namespace libchiaking::trophies
