/// @file
/// @brief Declares PlayerSessions, the session manager's game sessions that
/// invitations point at, read with the app token to decide whether an
/// invitation can be joined.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief How many sessions one request reads.
inline constexpr std::size_t player_sessions_batch = 10;

/// @brief A player in a game session. Plain data.
struct SessionPlayer {
  /// @brief The `accountId` in decimal; empty when absent.
  std::string account_id;
  /// @brief The `onlineId`; empty when absent.
  std::string online_id;
};

/// @brief A game session as the session manager describes it. Plain data;
/// text fields are empty when PS omitted them.
struct PlayerSession {
  /// @brief The `sessionId`.
  std::string session_id;
  /// @brief The `sessionName`.
  std::string name;
  /// @brief The `maxPlayers`; zero when absent.
  std::uint32_t max_players{};
  /// @brief The PSN players in `member.players`, in reply order.
  std::vector<SessionPlayer> players;
  /// @brief How many `member.nonPsnPlayers` there are.
  std::size_t other_players{};
  /// @brief The games in `playerGameIntent.npTitleIds`, in reply order;
  /// empty unless the reply carries them.
  std::vector<std::string> title_ids;
  /// @brief The `tokenizedPlayerGameIntent` a console joins with; empty when
  /// absent.
  SecretString game_intent;
  /// @brief Whether anyone may join, which `joinableUserType` says with
  /// `ANYONE`; false for friends only, any other value or none.
  bool open_to_anyone{};
};

/// @brief Reads game sessions with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class PlayerSessions {
 public:
  /// @brief The failure of read().
  using ReadError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  PlayerSessions(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Reads up to player_sessions_batch sessions.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/sessionManager/v1/playerSessions?fields=...`
  /// with the fields list, an `Accept-Language` header and
  /// the ids comma-joined in `X-PSN-SESSION-MANAGER-SESSION-IDS`. A session
  /// that has ended is missing from the reply.
  ///
  /// @param session_ids The sessions; only the first player_sessions_batch
  ///     are read.
  /// @param language The locale to send, such as `en-GB`.
  /// @return The sessions found, in reply order; nothing is asked when
  ///     `session_ids` is empty.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto read(std::span<const std::string> session_ids,
                          std::string_view language)
      -> Expected<std::vector<PlayerSession>, ReadError>;

 private:
  /// @brief The game sessions.
  static constexpr std::string_view sessions_url =
      "https://m.np.playstation.com/api/sessionManager/v1/playerSessions";
  /// @brief The fields asked for. `playerGameIntent` is left out: the
  /// session manager refuses it to the app token.
  static constexpr std::string_view fields =
      "sessionId,createdTimestamp,maxPlayers,member(players),"
      "member(nonPsnPlayers),member(spectators),supportedPlatforms,"
      "sessionName,leader,joinableUserType,invitableUserType,"
      "tokenizedPlayerGameIntent,tokenizedSpectatorGameIntent";
  /// @brief The header that names the sessions to read.
  static constexpr std::string_view session_ids_header =
      "X-PSN-SESSION-MANAGER-SESSION-IDS";

  /// @brief The request, logged with its query.
  static constexpr net::http::Endpoint read_endpoint{
      .label = "psn.session_manager.player_sessions",
      .log_query = true,
  };

  /// @brief The reply's `playerSessions`.
  struct Reply;

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
