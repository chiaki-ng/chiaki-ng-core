/// @file
/// @brief Declares SessionManager, the client of PSN's session manager for
/// Remote Play sessions, and the identifiers it trades in.
///
/// Remote Play over the internet begins by creating a Remote Play session on
/// the session manager with the Remote Play token. The console joins it, and
/// the two then exchange holepunch offers as session messages through it until
/// a direct UDP path is found; the client leaves the session when done.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/wire_sizes.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::psn::rest {

/// @brief The console generation a session message is addressed to, sent as
/// its `platform`.
enum class ConsoleFamily : std::uint8_t {
  /// @brief A PS4, sent as `PS4`.
  ps4,
  /// @brief A PS5, sent as `PS5`.
  ps5
};

/// @brief How much of the session the observe request asks for.
enum class SessionView : std::uint8_t {
  /// @brief The default view; no `view` parameter is sent.
  status,
  /// @brief The detailed view, `view=v1.0`.
  detailed
};

/// @brief A session manager session id: 36 characters of ASCII letters,
/// digits and `-` (a UUID's form), checked before it is put in a URL. An
/// immutable value.
class SessionId {
 public:
  /// @brief The length of a session id, in characters.
  static constexpr std::size_t session_id_length = 36;

  /// @brief Checks a session id.
  ///
  /// @param text The id.
  /// @return The id, or nothing when it is not 36 letters, digits and
  ///     hyphens.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<SessionId>;

  /// @brief Reads the id.
  ///
  /// @return The id; valid while this object lives.
  auto view() const noexcept -> std::string_view { return value_; }

 private:
  /// @brief Wraps an id parse() has checked.
  ///
  /// @param text The id.
  explicit SessionId(std::string_view text);

  /// @brief The id.
  std::string value_;
};

/// @brief A newly created Remote Play session. Plain data.
struct CreatedSession {
  /// @brief The push context the session was created with: a version 4 UUID
  /// the client chose, naming its push channel to the session manager.
  std::string push_context_id;
  /// @brief The session's id, a valid SessionId.
  std::string session_id;
  /// @brief The creating member's account id as the session manager reports
  /// it.
  std::int64_t account_id{};
};

/// @brief The member a session message is addressed to. Plain data.
struct MessageRecipient {
  /// @brief The recipient's account id.
  std::int64_t account_id{};
  /// @brief The recipient console's device unique id; the bytes must outlive
  /// the send.
  std::span<const std::byte, psn_device_uid_size> device_uid;
  /// @brief The recipient console's generation.
  ConsoleFamily family{ConsoleFamily::ps5};
};

/// @brief Remote Play session requests to
/// `web.np.playstation.com/api/sessionManager/v1/remotePlaySessions`, each
/// with the Remote Play token as a bearer token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Every call blocks for one round trip and keeps the reply even if the stop
/// token fires mid-request, so a session created just as the user cancels is
/// still known and can be left. Not for concurrent use.
class SessionManager {
 public:
  /// @brief The fault of reading a created session with created_session().
  using CreatedFault =
      Fault<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidSessionId,
            bad_reply::InvalidAccountId>;
  /// @brief The failure of create_remote_play_session().
  using CreateError =
      Error<bad_request::InvalidMessage, bad_reply::TooLarge,
            bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidSessionId,
            bad_reply::InvalidAccountId>;
  /// @brief The failure of send_session_message().
  using SessionMessageError = Error<bad_request::InvalidMessage>;
  /// @brief The failure of leave_session().
  using LeaveError = Error<>;
  /// @brief The failure of observe_sessions().
  using ObserveError = Error<>;
  /// @brief The failure of an exchange with send().
  using ExchangeError = Error<>;
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The Remote Play token.
  SessionManager(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Creates a Remote Play session with the user as its only member.
  ///
  /// POST the collection with
  /// `{"remotePlaySessions":[{"members":[{"accountId":
  /// "me","deviceUniqueId":"me","platform":"me","pushContexts":[{
  /// "pushContextId":<uuid>}]}]}]}`; the reply is read in PS's wire dialect
  /// for the first session's `sessionId` and its first member's `accountId`
  /// (a number or a quoted number).
  ///
  /// @param random 16 random bytes the push context UUID is made from.
  /// @return The session.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval bad_reply::InvalidSchema A session, its id, its members or
  ///     the account id is missing.
  /// @retval bad_reply::InvalidSessionId The session id is not a
  ///     SessionId.
  /// @retval bad_reply::InvalidAccountId The account id is not a 64-bit
  ///     integer.
  /// @retval Error A transport failure, an error status, or a parse failure.
  [[nodiscard]] auto create_remote_play_session(
      std::span<const std::byte, 16> random)
      -> Expected<CreatedSession, CreateError>;
  /// @brief Asks the session manager about a session while waiting for the
  /// console to join; the reply's body is not read.
  ///
  /// GET the collection, with `view=v1.0` for the detailed view, naming the
  /// session in the `X-PSN-SESSION-MANAGER-SESSION-IDS` header.
  ///
  /// @param session The session.
  /// @param view How much to ask for.
  /// @return Nothing, on a 2xx reply.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto observe_sessions(const SessionId& session,
                                      SessionView view)
      -> Expected<void, ObserveError>;
  /// @brief Takes the user out of a session.
  ///
  /// DELETE `<collection>/<session id>/members/me` with an empty JSON body.
  ///
  /// @param session The session.
  /// @return Nothing, on a 2xx reply.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto leave_session(const SessionId& session)
      -> Expected<void, LeaveError>;
  /// @brief Sends a member of the session a message, such as a holepunch
  /// offer.
  ///
  /// POST `<collection>/<session id>/sessionMessage` with
  /// `{"channel":"remote_play:1","payload":"ver=1.0, type=text,
  /// body=<payload>",
  /// "to":[{"accountId":"<id>","deviceUniqueId":"<hex>","platform":"PS4|PS5"}]}`.
  ///
  /// @param session The session.
  /// @param payload The message text.
  /// @param recipient Whom it is for.
  /// @return Nothing, on a 2xx reply.
  /// @retval bad_request::InvalidMessage The body could not be written or
  ///     is larger than 33 KiB.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto send_session_message(const SessionId& session,
                                          std::string_view payload,
                                          const MessageRecipient& recipient)
      -> Expected<void, SessionMessageError>;

 private:
  /// @brief The HTTP methods.
  using Method = net::http::Method;
  /// @brief The Remote Play sessions collection.
  static constexpr std::string_view sessions_url =
      "https://web.np.playstation.com/api/sessionManager/v1/remotePlaySessions";
  /// @brief The content type of every body sent.
  static constexpr std::string_view content_type =
      "application/json; charset=utf-8";
  /// @brief What every session message payload starts with.
  static constexpr std::string_view message_prefix =
      "ver=1.0, type=text, body=";
  /// @brief The largest session message body sent, in bytes.
  static constexpr std::size_t maximum_message_bytes = 33U * 1024U;

  /// @brief The create request.
  static constexpr net::http::Endpoint create_endpoint{
      .label = "psn.session_manager.create_remote_play_session",
      .method = Method::post,
      .keep_reply_after_stop = true,
  };

  /// @brief The observe request, logged with its query.
  static constexpr net::http::Endpoint observe_endpoint{
      .label = "psn.session_manager.observe_sessions",
      .log_query = true,
      .keep_reply_after_stop = true,
  };

  /// @brief The leave request.
  static constexpr net::http::Endpoint leave_endpoint{
      .label = "psn.session_manager.leave_session",
      .method = Method::delete_,
      .keep_reply_after_stop = true,
  };

  /// @brief The session message request.
  static constexpr net::http::Endpoint message_endpoint{
      .label = "psn.session_manager.send_session_message",
      .method = Method::post,
      .keep_reply_after_stop = true,
  };

  /// @brief The create request's body.
  struct CreateBody;
  /// @brief The create reply's fields.
  struct CreatedReply;
  /// @brief The session message request's body.
  struct MessageBody;

  /// @brief Builds a URL under one session.
  ///
  /// @param session The session.
  /// @param suffix What follows the session id, such as `/members/me`.
  /// @return The URL.
  static auto session_url(const SessionId& session, std::string_view suffix)
      -> std::string;
  /// @brief Reads an account id that may be a JSON number or a quoted one.
  ///
  /// @param raw The JSON value as written.
  /// @return The id, or nothing when it is not a 64-bit integer or a quoted
  ///     one starts with `+`.
  [[nodiscard]] static auto account_number(std::string_view raw)
      -> std::optional<std::int64_t>;
  /// @brief Reads a create reply.
  ///
  /// @param push_context The push context id that was sent.
  /// @param body The reply body.
  /// @return The session.
  /// @retval Error The response failures create_remote_play_session() lists.
  [[nodiscard]] static auto created_session(std::string push_context,
                                            std::string_view body)
      -> Expected<CreatedSession, CreatedFault>;

  /// @brief Sends a request and accepts only a 2xx reply.
  ///
  /// @param request The request.
  /// @return The reply.
  /// @retval Error A transport failure or an error status, the token redacted
  ///     from the excerpt.
  [[nodiscard]] auto send(net::http::Request request)
      -> Expected<net::http::Response, ExchangeError>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The Remote Play token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
