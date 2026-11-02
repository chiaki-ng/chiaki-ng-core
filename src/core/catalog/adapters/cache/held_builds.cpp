#include "src/core/catalog/adapters/cache/held_builds.hpp"

#include <utility>

namespace libchiaking::catalog::cache {

auto HeldBuilds::matched_before(AccountId account, const MatchedFrom& sources,
                                const MyGames::Inputs& inputs) const
    -> GamesPtr {
  const auto found = builds_.find(account.value());
  if (found == builds_.end()) return nullptr;
  const auto& held = found->second;
  if (held.games && same_sources(held.matched, sources) &&
      held.games->was_computed_from(inputs))
    return held.games;
  return nullptr;
}

void HeldBuilds::keep(AccountId account, const GamesPtr& games,
                      MatchedFrom sources) {
  auto& held = builds_[account.value()];
  held.games = games;
  held.matched = std::move(sources);
}

void HeldBuilds::forget(AccountId account) { builds_.erase(account.value()); }

auto HeldBuilds::same_sources(const MatchedFrom& held,
                              const MatchedFrom& asked) noexcept -> bool {
  return held.list == asked.list && held.titles == asked.titles &&
         held.ps5 == asked.ps5 && held.category == asked.category &&
         held.for_sale == asked.for_sale &&
         gates_alike(held.streaming, asked.streaming);
}

auto HeldBuilds::gates_alike(
    const std::optional<users::CloudStreaming>& held,
    const std::optional<users::CloudStreaming>& asked) noexcept -> bool {
  const auto allows =
      [](const std::optional<users::CloudStreaming>& streaming) {
        return streaming && streaming->is_allowed();
      };
  const auto hides = [](const std::optional<users::CloudStreaming>& streaming) {
    return streaming && streaming->hides_tiles();
  };
  return allows(held) == allows(asked) && hides(held) == hides(asked);
}

}  // namespace libchiaking::catalog::cache
