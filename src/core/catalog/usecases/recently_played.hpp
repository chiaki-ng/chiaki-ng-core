/// @file
/// @brief Declares usecases::RecentlyPlayed, the use case that lists the cloud
/// games the user played most recently.
///
/// PSN's played titles are joined to the shown games by title number; "for
/// sale" tiles are left out.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/detail/catalog/my_games.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Joins PSN's played titles to the user's shown games.
///
/// Borrows the provider, log and ListGames, which must outlive it. Not
/// copyable.
class RecentlyPlayed {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;

  /// @brief The most games returned.
  static constexpr std::size_t most_shown = 20;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends the played-titles request. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param list Lists the user's games. Must outlive this object.
  RecentlyPlayed(platform::HttpProvider& http,
                 const platform::ClockProvider& clock,
                 platform::DiagnosticsProvider* log, ListGames& list) noexcept;
  /// @brief Not copyable: it borrows shared state.
  RecentlyPlayed(const RecentlyPlayed&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const RecentlyPlayed&) -> RecentlyPlayed& = delete;

  /// @brief The user's recently played cloud games.
  ///
  /// Blocks while the games are listed (see ListGames::games) and the played
  /// titles are read.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return Up to most_shown games in PSN's order; the sign-in failure; the
  ///     games' failure; the played-titles failure.
  [[nodiscard]] auto games(users::PsUser& user, const Stop& stop)
      -> Expected<std::vector<RecentlyPlayedGame>, Unanswered>;

  /// @brief Joins played titles to shown games.
  ///
  /// Each played title matches the first visible game, not a "for sale" tile,
  /// whose product carries the same title number; a game is listed once.
  ///
  /// @param games The user's games.
  /// @param played PSN's played titles, most recent first.
  /// @return Up to most_shown games with their last-played times and what
  ///     PSN reported about the matching title.
  static auto matched(const MyGames& games,
                      std::span<const psn::rest::PlayedTitle> played)
      -> std::vector<RecentlyPlayedGame>;

 private:
  /// @brief Whether a game may appear as recently played.
  ///
  /// @param game The game.
  /// @return False for a store-only tile.
  static auto playable(const MyGame& game) noexcept -> bool;
  /// @brief Builds the answer for a game matched by a played title.
  ///
  /// @param game The matched game.
  /// @param title The played title.
  /// @return The game with what PSN reported about the title.
  static auto entry_of(const MyGame& game, const psn::rest::PlayedTitle& title)
      -> RecentlyPlayedGame;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Lists the user's games; not owned.
  ListGames& list_;
};

}  // namespace libchiaking::catalog::usecases
