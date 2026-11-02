/// @file
/// @brief Declares Gamelist, the PSN game list service's record of the games
/// the account has played, newest first, read with the app token for the
/// recently played rail.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief How many titles one page asks for.
inline constexpr std::size_t played_titles_page_size = 50;

/// @brief A game the account has played. Plain data; text fields are empty
/// when PS omitted them.
struct PlayedTitle {
  /// @brief The `titleId`, such as `PPSA01234_00`.
  std::string title_id;
  /// @brief The `localizedName`, or the `name` when that is blank.
  std::string name;
  /// @brief The `localizedImageUrl`, or the `imageUrl` when that is blank.
  std::string image_url;
  /// @brief The `category`, such as `ps5_native_game`.
  std::string category;
  /// @brief The `lastPlayedDateTime` as PS wrote it.
  std::string last_played;
  /// @brief The `firstPlayedDateTime` as PS wrote it.
  std::string first_played;
  /// @brief The `playDuration`, an ISO 8601 duration such as `PT12H34M5S`,
  /// as PS wrote it.
  std::string play_duration;
  /// @brief The `playCount`; nothing when PS omitted it or sent no
  /// readable count.
  std::optional<std::uint32_t> play_count;
};

/// @brief One page of played games. Plain data.
struct PlayedTitlesPage {
  /// @brief The readable titles, in reply order.
  std::vector<PlayedTitle> titles;
  /// @brief How many entries the page held, readable or not; fewer than
  /// played_titles_page_size means the last page.
  std::size_t received{};
  /// @brief The `nextOffset`; nothing when PS gave none.
  std::optional<std::size_t> next_offset;
  /// @brief The `totalItemCount`, when PS gave it.
  std::optional<std::size_t> total;
};

/// @brief Reads the played games with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class Gamelist {
 public:
  /// @brief The failure of played_titles().
  using PlayedTitlesError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  Gamelist(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Reads one page of played games.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/gamelist/v2/users/me/titles?limit=50&offset=<offset>`.
  ///
  /// @param offset Index of the first title.
  /// @return The page.
  /// @retval bad_reply::InvalidSchema The reply has no `titles`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto played_titles(std::size_t offset)
      -> Expected<PlayedTitlesPage, PlayedTitlesError>;

 private:
  /// @brief The account's played titles.
  static constexpr std::string_view titles_url =
      "https://m.np.playstation.com/api/gamelist/v2/users/me/titles";

  /// @brief The request, logged with its query.
  static constexpr net::http::Endpoint played_titles_endpoint{
      .label = "psn.gamelist.played_titles",
      .log_query = true,
  };

  /// @brief The reply's `titles`.
  struct Reply;

  /// @brief Picks the localised text when it is not blank.
  ///
  /// @param preferred The localised field, moved from when chosen.
  /// @param fallback The plain field, moved from when chosen.
  /// @return The preferred text, else the fallback, else empty.
  static auto first_present(json::ps::Tolerant<std::string>& preferred,
                            json::ps::Tolerant<std::string>& fallback)
      -> std::string;

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
