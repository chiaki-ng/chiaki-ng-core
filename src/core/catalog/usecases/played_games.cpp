#include "src/core/catalog/usecases/played_games.hpp"

#include "src/core/catalog/usecases/asker.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/utils/json/ps_fields.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

PlayedGames::PlayedGames(platform::HttpProvider& http,
                         const platform::ClockProvider& clock,
                         platform::DiagnosticsProvider* log, HeldAnswers& held,
                         users::RequestPacer& pacer) noexcept
    : http_(http), clock_(clock), log_(log), held_(held), pacer_(pacer) {}

auto PlayedGames::games(users::PsUser& user, const Stop& stop)
    -> Expected<std::vector<PlayedGame>, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  auto held = HeldAnswers::answer(
      held_.in_flight().played, HeldAnswers::Question::by(account), stop,
      [&] {
        auto kept = held_.fresh_played(account);
        return kept ? std::optional{std::move(kept)} : std::nullopt;
      },
      [&]() -> Expected<HeldAnswers::PlayedPtr, Unanswered> {
        auto read = fetched(user, stop);
        if (!read) return unexpected(read.error());
        return held_.keep_played(account, std::move(*read));
      });
  if (!held) return unexpected(held.error());
  return *held.value();
}

auto PlayedGames::fetched(users::PsUser& user, const Stop& stop)
    -> Expected<std::vector<PlayedGame>, Unanswered> {
  net::http::Client client{http_, clock_, stop, log_};
  users::CatalogCalls calls{user};
  const auto account = user.account_id();
  std::vector<PlayedGame> games;
  std::size_t offset = 0;
  for (int page = 0; page < page_limit; ++page) {
    if (auto turn = pacer_.admit(account, stop); !turn)
      return unexpected(Unanswered{turn.error()});
    auto read = calls.played_titles(client, offset);
    if (!read) {
      if (read.error().reason == users::Unanswered::Reason::rate_limited)
        pacer_.throttled(account);
      return unexpected(Unanswered{read.error()});
    }
    for (const auto& title : read->titles) {
      auto game = game_of(title);
      if (!game ||
          std::ranges::contains(games, game->title, &PlayedGame::title))
        continue;
      games.push_back(std::move(*game));
    }
    auto next = read->next_offset;
    if (!next && read->received == psn::rest::played_titles_page_size)
      next = offset + read->received;
    if (read->received == 0 || !next) {
      client.log(platform::LogSeverity::info,
                 "[PLAYED] " + std::to_string(games.size()) +
                     " played games over " + std::to_string(page + 1) +
                     " pages");
      return games;
    }
    if (*next <= offset)
      return unexpected(Unanswered{Unanswered::Reason::malformed});
    offset = *next;
  }
  return unexpected(Unanswered{Unanswered::Reason::malformed});
}

auto PlayedGames::game_of(const psn::rest::PlayedTitle& title)
    -> std::optional<PlayedGame> {
  auto id = NpTitleId::parse(text::trim(title.title_id));
  if (!id) return std::nullopt;
  return PlayedGame{.title = std::move(*id),
                    .name = title.name,
                    .platform = platform_of(title.category),
                    .category = title.category,
                    .image_url = title.image_url,
                    .play_time = play_time_of(title.play_duration),
                    .play_count = title.play_count,
                    .first_played = played_at(title.first_played),
                    .last_played = played_at(title.last_played)};
}

auto PlayedGames::played_at(std::string_view text) -> std::optional<Time> {
  const auto instant = json::ps::Instant::from_text(text::trim(text));
  if (!instant || instant->time() <= Time{}) return std::nullopt;
  return instant->time();
}

auto PlayedGames::play_time_of(std::string_view text)
    -> std::optional<std::chrono::seconds> {
  const auto elapsed = json::ps::Elapsed::from_text(text::trim(text));
  if (!elapsed) return std::nullopt;
  return elapsed->length();
}

auto PlayedGames::platform_of(std::string_view category)
    -> std::optional<Platform> {
  if (category.starts_with("ps5")) return Platform::ps5;
  if (category.starts_with("ps4")) return Platform::ps4;
  if (category.starts_with("ps3")) return Platform::ps3;
  return std::nullopt;
}

}  // namespace libchiaking::catalog::usecases
