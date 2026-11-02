/// @file
/// @brief Declares SessionInvitations, the session manager and share link
/// requests that find the user's game session, share it and invite friends
/// to a session or a party.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief A link that lets friends join a game session. Plain data.
struct SessionShareLink {
  /// @brief The `sessionJoinUrl` friends open.
  std::string url;
  /// @brief The `sessionJoinImageUrl`, a picture of the link; empty when
  /// absent.
  std::string image_url;
};

/// @brief Finds, shares and invites friends to the user's sessions, signed
/// with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class SessionInvitations {
 public:
  /// @brief The failure of invite_to_party() and invite_to_game().
  using InviteError = Error<bad_request::InvalidMessage>;
  /// @brief The failure of share_link().
  using ShareLinkError =
      Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
            bad_reply::InvalidSchema>;
  /// @brief The failure of joined().
  using JoinedError = Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
                            bad_reply::InvalidSchema>;
  /// @brief The most invitees one request carries.
  static constexpr std::size_t most_invitees = 16;

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  SessionInvitations(net::http::Client& client,
                     auth::BearerToken token) noexcept;

  /// @brief Lists the game sessions the user plays in for a title.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/sessionManager/v1/users/<account>/playerSessions`
  /// with `relatedCreatorNpTitleIdFilter`, `platformFilter` and `deviceType`,
  /// which finds the session of the game a user is playing.
  ///
  /// @param account_id The user's account id in decimal.
  /// @param title_id The title, such as `PPSA01234_00`.
  /// @param platform The game's platform, such as `PS5`.
  /// @param device_type The device the game runs on, such as `PS5`.
  /// @param language The locale to send, such as `en-GB`.
  /// @return The session ids in reply order.
  /// @retval bad_request::InvalidMessage An id or filter holds characters
  ///     a URL path or query cannot carry.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto joined(std::string_view account_id,
                            std::string_view title_id,
                            std::string_view platform,
                            std::string_view device_type,
                            std::string_view language)
      -> Expected<std::vector<std::string>, JoinedError>;

  /// @brief Asks for a link friends can open to join a game session.
  ///
  /// GET `https://m.np.playstation.com/api/cpss/v1/invite/<session>`.
  ///
  /// @param session_id The session.
  /// @param language The locale to send.
  /// @return The link.
  /// @retval bad_reply::InvalidSchema The reply has no
  ///     `sessionJoinUrl`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto share_link(std::string_view session_id,
                                std::string_view language)
      -> Expected<SessionShareLink, ShareLinkError>;

  /// @brief Invites accounts to a party.
  ///
  /// POST
  /// `https://m.np.playstation.com/api/sessionManager/v2/partySessions/<session>/invitations`
  /// with `{"invitations":[{"to":{"accountId"}}],"invitationType":"NORMAL"}`,
  /// a party invitation. The reply's body is not read.
  ///
  /// @param session_id The party.
  /// @param account_ids The invitees in decimal; at most most_invitees.
  /// @param language The locale to send.
  /// @return Nothing once PSN accepted the invitations.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto invite_to_party(std::string_view session_id,
                                     std::span<const std::string> account_ids,
                                     std::string_view language)
      -> Expected<void, InviteError>;

  /// @brief Invites accounts to a game session.
  ///
  /// POST
  /// `https://m.np.playstation.com/api/sessionManager/v1/playerSessions/<session>/invitations`
  /// with `{"invitations":[{"to":{"accountId"}}]}`. The body follows the party
  /// invitation's and is unconfirmed. The reply's body is not read.
  ///
  /// @param session_id The game session.
  /// @param account_ids The invitees in decimal; at most most_invitees.
  /// @param language The locale to send.
  /// @return Nothing once PSN accepted the invitations.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto invite_to_game(std::string_view session_id,
                                    std::span<const std::string> account_ids,
                                    std::string_view language)
      -> Expected<void, InviteError>;

 private:
  /// @brief The session manager.
  static constexpr std::string_view session_manager_url =
      "https://m.np.playstation.com/api/sessionManager";
  /// @brief The share link service.
  static constexpr std::string_view share_url =
      "https://m.np.playstation.com/api/cpss/v1/invite/";
  /// @brief The content type of an invitation.
  static constexpr std::string_view content_type =
      "application/json; charset=utf-8";

  /// @brief The joined sessions read.
  static constexpr net::http::Endpoint joined_endpoint{
      .label = "psn.session_manager.joined_player_sessions",
      .log_query = true,
  };
  /// @brief The share link read.
  static constexpr net::http::Endpoint share_endpoint{
      .label = "psn.cpss.session_share_link",
  };
  /// @brief The party invitation.
  static constexpr net::http::Endpoint party_endpoint{
      .label = "psn.session_manager.party_invitations",
      .method = net::http::Method::post,
  };
  /// @brief The game session invitation.
  static constexpr net::http::Endpoint game_endpoint{
      .label = "psn.session_manager.player_session_invitations",
      .method = net::http::Method::post,
  };

  /// @brief The joined sessions reply.
  struct Joined;
  /// @brief The share link reply.
  struct Shared;
  /// @brief An invitation body.
  struct Invitations;

  /// @brief Sends an invitation body.
  ///
  /// @param endpoint The request's endpoint.
  /// @param url The invitations URL.
  /// @param account_ids The invitees.
  /// @param invitation_type The `invitationType`, or empty to leave it out.
  /// @param language The locale to send.
  /// @return Nothing once PSN accepted it.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto invite(const net::http::Endpoint& endpoint,
                            std::string url,
                            std::span<const std::string> account_ids,
                            std::string_view invitation_type,
                            std::string_view language)
      -> Expected<void, InviteError>;

  /// @brief Whether text can stand in a URL path segment unescaped.
  ///
  /// @param text The text.
  /// @return True for 1 to 64 letters, digits, `-` and `_`.
  [[nodiscard]] static auto path_safe(std::string_view text) noexcept -> bool;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
