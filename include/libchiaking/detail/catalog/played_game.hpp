/// @file
/// @brief Declares PlayedGame, one console release of a game the account has
/// played, as PSN's play history reports it.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::catalog {

/// @brief One console release of a game the account has played, whether or
/// not it streams. Plain data.
///
/// Text fields are empty and optional fields empty when PSN gave nothing
/// readable for them.
///
/// @see Catalog::played_games
struct PlayedGame {
  /// @brief The release, which other PSN services, such as the trophy
  /// service, know the game by.
  NpTitleId title;
  /// @brief The localised name, or the plain name when that is blank.
  std::string name;
  /// @brief The console the release is for, read from PSN's category.
  std::optional<Platform> platform;
  /// @brief PSN's category, such as `ps5_native_game` or `ps4_game`.
  std::string category;
  /// @brief The localised artwork address, or the plain one when that is
  /// blank.
  std::string image_url;
  /// @brief How long the account has played the release in all, to the
  /// second.
  std::optional<std::chrono::seconds> play_time;
  /// @brief How many times PSN saw the release started.
  std::optional<std::uint32_t> play_count;
  /// @brief When PSN first saw the release played.
  std::optional<Time> first_played;
  /// @brief When PSN last saw the release played.
  std::optional<Time> last_played;

  /// @brief Compares every field.
  friend auto operator==(const PlayedGame&, const PlayedGame&)
      -> bool = default;
};

}  // namespace libchiaking::catalog
