/// @file
/// @brief Declares usecases::PlayedGames, the use case that reads every game
/// the account has played from PSN's game list, page by page, and keeps it.
#pragma once

#include "libchiaking/detail/catalog/played_game.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/users/request_pacer.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Reads the account's whole play history, kept for six hours.
///
/// Borrows the providers and the held answers, which must outlive it. Callers
/// asking for the same account at the same time share one read. Not copyable.
class PlayedGames {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;

  /// @brief Most pages read before the history counts as malformed.
  static constexpr int page_limit = 50;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends the game list requests. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where the history is kept. Must outlive this object.
  /// @param pacer Paces each page against the account's request budget.
  ///     Must outlive this object.
  PlayedGames(platform::HttpProvider& http,
              const platform::ClockProvider& clock,
              platform::DiagnosticsProvider* log, HeldAnswers& held,
              users::RequestPacer& pacer) noexcept;
  /// @brief Not copyable: it borrows shared state.
  PlayedGames(const PlayedGames&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const PlayedGames&) -> PlayedGames& = delete;

  /// @brief Every game the account has played, kept or read.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return The games in PSN's order; the failures Catalog::played_games
  ///     lists.
  [[nodiscard]] auto games(users::PsUser& user, const Stop& stop)
      -> Expected<std::vector<PlayedGame>, Unanswered>;

  /// @brief Reads one played title into a game.
  ///
  /// @param title The title as the game list gave it.
  /// @return The game, or nothing when its title id is not readable.
  static auto game_of(const psn::rest::PlayedTitle& title)
      -> std::optional<PlayedGame>;
  /// @brief Reads a play time stamp.
  ///
  /// @param text The time as PSN wrote it.
  /// @return The time, or nothing when it does not parse or is not after the
  ///     epoch.
  static auto played_at(std::string_view text) -> std::optional<Time>;
  /// @brief Reads a total play time.
  ///
  /// @param text The ISO 8601 duration as PSN wrote it.
  /// @return The time to the second, or nothing when it does not parse.
  static auto play_time_of(std::string_view text)
      -> std::optional<std::chrono::seconds>;

 private:
  /// @brief Reads every page of the game list.
  ///
  /// @param user The user.
  /// @param stop Abandons the reads.
  /// @return The games, each title id once, in PSN's order.
  /// @retval Unanswered::Reason::malformed A page's next offset did not move
  ///     past the one asked for, or page_limit pages were read.
  /// @retval Unanswered The failures of a page's turn or request; a 429
  ///     starts the account's cooldown.
  [[nodiscard]] auto fetched(users::PsUser& user, const Stop& stop)
      -> Expected<std::vector<PlayedGame>, Unanswered>;
  /// @brief Reads the console from PSN's category.
  ///
  /// @param category The category, such as `ps5_native_game`.
  /// @return The console its prefix names, or nothing for another one.
  static auto platform_of(std::string_view category) -> std::optional<Platform>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Where the history is kept; not owned.
  HeldAnswers& held_;
  /// @brief Paces each page; not owned.
  users::RequestPacer& pacer_;
};

}  // namespace libchiaking::catalog::usecases
