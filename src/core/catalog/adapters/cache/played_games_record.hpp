/// @file
/// @brief Declares cache::PlayedGamesRecord, the stored form of one account's
/// whole play history.
///
/// Stored at user/{account}/played_games and served for six hours, so that a
/// restart or a second screen does not page through the game list again.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"

#include <libchiaking/detail/catalog/played_game.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief One account's stored play history.
///
/// Immutable; may be shared between threads.
class PlayedGamesRecord {
 public:
  /// @brief Records are keyed by account.
  using Key = AccountId;

  /// @brief The stored form of a PlayedGame. Plain data.
  struct Game {
    /// @brief The title id.
    std::string title;
    /// @brief The name.
    std::string name;
    /// @brief The platform, when known.
    std::optional<Platform> platform;
    /// @brief PSN's category.
    std::string category;
    /// @brief The artwork address.
    std::string image_url;
    /// @brief The total play time in seconds, when known.
    std::optional<std::int64_t> play_seconds;
    /// @brief The play count, when known.
    std::optional<std::uint32_t> play_count;
    /// @brief The first play; wall-clock milliseconds.
    std::optional<std::int64_t> first_played_ms;
    /// @brief The last play; wall-clock milliseconds.
    std::optional<std::int64_t> last_played_ms;
  };

  /// @brief The stored body. Plain data.
  struct Body {
    /// @brief When the history was read; wall-clock milliseconds.
    std::int64_t fetched_at_ms{};
    /// @brief Every game, in PSN's order.
    std::vector<Game> games;
  };

  /// @brief The record kind and last name segment.
  static constexpr std::string_view kind = "played_games";
  /// @brief The schema version.
  static constexpr std::int64_t schema = 1;
  /// @brief How long a history is served after it was read.
  static constexpr std::chrono::hours lifetime{6};
  /// @brief How long the record is kept on disk after it was written; an
  /// older one is discarded on read.
  static constexpr auto kept_for = lifetime;

  /// @brief Wraps a history for storing.
  ///
  /// @param account The account it belongs to.
  /// @param fetched_at When it was read.
  /// @param games The games, in PSN's order.
  PlayedGamesRecord(AccountId account, Time fetched_at,
                    std::vector<PlayedGame> games) noexcept;

  /// @brief Reads a stored body back.
  ///
  /// @param body The stored body.
  /// @param account The account the record is stored under.
  /// @return The record; std::nullopt when any game's title id does not
  ///     read or a play time is negative.
  [[nodiscard]] static auto from_body(const Body& body, AccountId account)
      -> std::optional<PlayedGamesRecord>;
  /// @brief The record's name.
  ///
  /// @param account The account.
  /// @return user/{account}/played_games.
  [[nodiscard]] static auto name_of(AccountId account)
      -> std::optional<platform::RecordName>;

  /// @brief The record's key.
  ///
  /// @return The account.
  auto key() const noexcept -> AccountId { return account_; }
  /// @brief When the history was read.
  ///
  /// @return The wall-clock time.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief The games.
  ///
  /// @return The games in PSN's order, valid while the record lives.
  auto games() const noexcept -> const std::vector<PlayedGame>& {
    return games_;
  }
  /// @brief The stored body.
  ///
  /// @return The body.
  auto body() const -> Body;

  /// @brief Compares every field.
  friend auto operator==(const PlayedGamesRecord&, const PlayedGamesRecord&)
      -> bool = default;

 private:
  /// @brief The stored form of a game.
  ///
  /// @param game The game.
  /// @return Its stored form.
  static auto stored(const PlayedGame& game) -> Game;
  /// @brief A stored game read back.
  ///
  /// @param wire The stored game.
  /// @return The game, or std::nullopt when its title id does not read or
  ///     its play time is negative.
  [[nodiscard]] static auto game_of(const Game& wire)
      -> std::optional<PlayedGame>;

  /// @brief The account the history belongs to.
  AccountId account_;
  /// @brief When the history was read.
  Time fetched_at_;
  /// @brief The games.
  std::vector<PlayedGame> games_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How a stored played game is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::PlayedGamesRecord::Game> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::PlayedGamesRecord::Game;
  /// @brief The JSON keys title, name, platform, category, imageUrl,
  /// playSeconds, playCount, firstPlayedMs and lastPlayedMs.
  static constexpr auto value = glz::object(
      "title", &T::title, "name", &T::name, "platform", &T::platform,
      "category", &T::category, "imageUrl", &T::image_url, "playSeconds",
      &T::play_seconds, "playCount", &T::play_count, "firstPlayedMs",
      &T::first_played_ms, "lastPlayedMs", &T::last_played_ms);
};

/// @brief How a played games record body is spelled in JSON.
template <>
struct glz::meta<libchiaking::catalog::cache::PlayedGamesRecord::Body> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::PlayedGamesRecord::Body;
  /// @brief The JSON keys fetchedAtMs and games.
  static constexpr auto value =
      glz::object("fetchedAtMs", &T::fetched_at_ms, "games", &T::games);
};
