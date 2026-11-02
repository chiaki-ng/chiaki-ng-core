#include "src/core/catalog/usecases/recently_played.hpp"
#include "src/core/catalog/usecases/played_games.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

RecentlyPlayed::RecentlyPlayed(platform::HttpProvider& http,
                               const platform::ClockProvider& clock,
                               platform::DiagnosticsProvider* log,
                               ListGames& list) noexcept
    : http_(http), clock_(clock), log_(log), list_(list) {}

auto RecentlyPlayed::games(users::PsUser& user, const Stop& stop)
    -> Expected<std::vector<RecentlyPlayedGame>, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  auto games = list_.games(user, stop);
  if (!games) return unexpected(games.error());
  net::http::Client client{http_, clock_, stop, log_};
  auto played = users::CatalogCalls{user}.played_titles(client, 0);
  if (!played) return unexpected(Unanswered{played.error()});
  auto found = matched(*games.value(), played.value().titles);
  client.log(platform::LogSeverity::info,
             "[RECENT] " + std::to_string(played.value().titles.size()) +
                 " played titles -> " + std::to_string(found.size()) +
                 " cloud games");
  return found;
}

auto RecentlyPlayed::playable(const MyGame& game) noexcept -> bool {
  return game.standing() != Standing::store;
}

auto RecentlyPlayed::entry_of(const MyGame& game,
                              const psn::rest::PlayedTitle& title)
    -> RecentlyPlayedGame {
  return {
      .product = game.product_id(),
      .last_played = PlayedGames::played_at(title.last_played).value_or(Time{}),
      .title = NpTitleId::parse(text::trim(title.title_id)),
      .play_time = PlayedGames::play_time_of(title.play_duration),
      .first_played = PlayedGames::played_at(title.first_played),
      .play_count = title.play_count};
}

auto RecentlyPlayed::matched(const MyGames& games,
                             std::span<const psn::rest::PlayedTitle> played)
    -> std::vector<RecentlyPlayedGame> {
  std::vector<RecentlyPlayedGame> found;
  for (const auto& title : played) {
    if (found.size() == most_shown) break;
    const auto number = TitleNumber::from(title.title_id);
    if (!number) continue;
    const auto shown = games.visible();
    const auto game = std::ranges::find_if(shown, [&](const MyGame& candidate) {
      const auto own = candidate.product_id().title_number();
      return own && *own == *number && playable(candidate);
    });
    if (game == shown.end()) continue;
    const auto already =
        std::ranges::any_of(found, [&](const RecentlyPlayedGame& entry) {
          return entry.product == game->product_id();
        });
    if (already) continue;
    found.push_back(entry_of(*game, title));
  }
  return found;
}

}  // namespace libchiaking::catalog::usecases
