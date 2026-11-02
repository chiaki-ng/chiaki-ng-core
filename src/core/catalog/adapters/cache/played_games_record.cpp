#include "src/core/catalog/adapters/cache/played_games_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"

#include <utility>

namespace libchiaking::catalog::cache {

PlayedGamesRecord::PlayedGamesRecord(AccountId account, Time fetched_at,
                                     std::vector<PlayedGame> games) noexcept
    : account_(account), fetched_at_(fetched_at), games_(std::move(games)) {}

auto PlayedGamesRecord::name_of(AccountId account)
    -> std::optional<platform::RecordName> {
  return Cache::user_record(account, kind);
}

auto PlayedGamesRecord::stored(const PlayedGame& game) -> Game {
  std::optional<std::int64_t> seconds;
  if (game.play_time) seconds = game.play_time->count();
  std::optional<std::int64_t> first;
  if (game.first_played) first = milliseconds_of(*game.first_played);
  std::optional<std::int64_t> last;
  if (game.last_played) last = milliseconds_of(*game.last_played);
  return Game{.title = game.title.text(),
              .name = game.name,
              .platform = game.platform,
              .category = game.category,
              .image_url = game.image_url,
              .play_seconds = seconds,
              .play_count = game.play_count,
              .first_played_ms = first,
              .last_played_ms = last};
}

auto PlayedGamesRecord::game_of(const Game& wire) -> std::optional<PlayedGame> {
  auto title = NpTitleId::parse(wire.title);
  if (!title || (wire.play_seconds && *wire.play_seconds < 0))
    return std::nullopt;
  std::optional<std::chrono::seconds> play_time;
  if (wire.play_seconds) play_time = std::chrono::seconds{*wire.play_seconds};
  return PlayedGame{.title = std::move(*title),
                    .name = wire.name,
                    .platform = wire.platform,
                    .category = wire.category,
                    .image_url = wire.image_url,
                    .play_time = play_time,
                    .play_count = wire.play_count,
                    .first_played = time_of(wire.first_played_ms),
                    .last_played = time_of(wire.last_played_ms)};
}

auto PlayedGamesRecord::from_body(const Body& body, AccountId account)
    -> std::optional<PlayedGamesRecord> {
  auto games = every_read<PlayedGame>(body.games, &PlayedGamesRecord::game_of);
  if (!games) return std::nullopt;
  return PlayedGamesRecord{account, time_of(body.fetched_at_ms),
                           std::move(*games)};
}

auto PlayedGamesRecord::body() const -> Body {
  Body body{.fetched_at_ms = milliseconds_of(fetched_at_), .games = {}};
  body.games.reserve(games_.size());
  for (const auto& game : games_) body.games.push_back(stored(game));
  return body;
}

}  // namespace libchiaking::catalog::cache
