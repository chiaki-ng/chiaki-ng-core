#include "src/core/catalog/adapters/cache/held_plays.hpp"

#include "src/core/catalog/adapters/cache/played_games_record.hpp"

#include <utility>

namespace libchiaking::catalog::cache {

auto HeldPlays::is_fresh(Time read_at, Time now) -> bool {
  return now >= read_at && now - read_at < PlayedGamesRecord::lifetime;
}

auto HeldPlays::fresh(AccountId account, Time now) const -> PlayedPtr {
  const auto found = played_.find(account.value());
  if (found != played_.end() && found->second.games &&
      is_fresh(found->second.read_at, now))
    return found->second.games;
  return nullptr;
}

void HeldPlays::hold(AccountId account, PlayedPtr games, Time read_at) {
  played_.insert_or_assign(account.value(), Read{std::move(games), read_at});
}

void HeldPlays::drop_everyone() { played_.clear(); }

void HeldPlays::forget(AccountId account) { played_.erase(account.value()); }

}  // namespace libchiaking::catalog::cache
