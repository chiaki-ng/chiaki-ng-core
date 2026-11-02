/// @file
/// @brief Declares the PSN push notification service: the request that finds
/// the push server, the WebSocket request that opens the push channel, and
/// Notification, one message received on it.
///
/// Remote Play over the internet signals through PS's session manager, and
/// the session manager tells each member about changes (a session was created,
/// a member joined, customData1 changed, a session message arrived) by push
/// notifications on a WebSocket channel authorised with the Remote Play
/// token. The same channel opened as a mobile client, with the app token,
/// also carries account alerts, such as game invitations.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/detail/wire_sizes.hpp>
#include <libchiaking/platform/websocket/provider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::psn::rest {

/// @brief The session manager event a push notification announces, from its
/// `dataType`.
enum class NotificationType : std::uint8_t {
  /// @brief `psn:sessionManager:sys:remotePlaySession:created`: a Remote Play
  /// session the user belongs to was created.
  remote_play_session_created,
  /// @brief `psn:sessionManager:sys:rps:members:created`: a member, such as
  /// the console, joined the session.
  members_created,
  /// @brief `psn:sessionManager:sys:rps:customData1:updated`: the session's
  /// customData1 changed.
  custom_data1_updated,
  /// @brief `psn:sessionManager:sys:rps:sessionMessage:created`: a member
  /// sent the session a message, such as a holepunch offer.
  session_message_created,
  /// @brief `psn:sessionManager:sys:rps:members:deleted`: a member left.
  members_deleted,
  /// @brief `psn:sessionManager:sys:remotePlaySession:deleted`: the session
  /// was deleted.
  remote_play_session_deleted,
  /// @brief `psn:sessionManager:sys:v2:ps:invitations:created`: someone
  /// invited the user to a session that a mobile client can join, which for
  /// a PS5 sender is a party; game invitations are never pushed. Sent only to
  /// a channel opened as a mobile client, twice for one invitation.
  invitation_created,
  /// @brief `np:service:gamingLoungeGroups`: a message group the user is in
  /// changed, such as a message arriving. Sent only to a channel opened as
  /// a mobile client.
  lounge_group_changed,
  /// @brief Any other `dataType`, none, or a body that is not JSON.
  unknown,
};

/// @brief Which client the push channel is opened as, which decides what PSN
/// sends on it.
enum class PushApp : std::uint8_t {
  /// @brief Remote Play push, with the Remote Play token: session
  /// manager changes of Remote Play sessions only.
  remote_play,
  /// @brief A mobile client, with the app token and the Android mobile
  /// headers: also game invitations and messages.
  mobile,
};

/// @brief A message someone sent the user, as its push names it. The push
/// carries no text.
struct LoungeMessage {
  /// @brief The sender's online id.
  std::string sender;
  /// @brief The message group it was sent in.
  std::string group_id;
  /// @brief The thread it was sent in; empty when the push named none.
  std::string thread_id;
  /// @brief The message's id.
  std::string message_uid;
};

/// @brief A session invitation, as its push names it.
struct SessionInvitation {
  /// @brief The inviter's online id.
  std::string sender;
  /// @brief The session the user was invited to.
  std::string session_id;
};

/// @brief Finds the push server.
class Push;

/// @brief One push notification as received, with its type and typed readers
/// for the fields each type carries.
///
/// The body is kept secret (it can hold session messages) and is rewritten
/// once into canonical JSON (json::WireDialect), which the readers decode. An
/// immutable value; safe to read from several threads.
class Notification {
 public:
  /// @brief The fault of reading a notification as one type's message:
  /// session_message_payload(), lounge_message(), session_invitation().
  using MessageFault = Fault<bad_reply::TooLarge, bad_reply::TooDeep,
                             bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The fault of session_created_online_id().
  using OnlineIdFault =
      Fault<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidOnlineId>;
  /// @brief The fault of member_device_uid().
  using DeviceUidFault =
      Fault<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidDeviceUid>;
  /// @brief The fault of custom_data1().
  using CustomData1Fault =
      Fault<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidCustomData1>;
  /// @brief Reads a notification frame.
  ///
  /// @param payload The WebSocket message's bytes.
  /// @return The notification; one whose body is not JSON is of type unknown
  ///     and its readers fail with the parse error.
  static auto parse(std::span<const std::byte> payload) -> Notification;
  /// @brief Reads a notification frame given as text.
  ///
  /// @param payload The message.
  /// @return As the byte overload.
  static auto parse(std::string_view payload) -> Notification;

  /// @brief Reads the type.
  ///
  /// @return The type.
  auto type() const noexcept -> NotificationType { return type_; }
  /// @brief Reads the `dataType`, which is not secret.
  ///
  /// @return The type as PS named it; empty when there is none or the body
  ///     is not JSON.
  auto data_type() const -> std::string;
  /// @brief Reads the body as received.
  ///
  /// @return The body; secret; valid while this object lives.
  auto body() const noexcept -> std::string_view { return body_.view(); }
  /// @brief Measures the body.
  ///
  /// @return Its size in bytes.
  auto size() const noexcept -> std::size_t { return body_.size(); }

  /// @brief Reads whom a session-created notification is addressed to,
  /// `to.onlineId`.
  ///
  /// @return The online id.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type or has no `to.onlineId`.
  /// @retval bad_reply::InvalidOnlineId The online id is not
  ///     acceptable.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto session_created_online_id() const
      -> Expected<std::string, OnlineIdFault>;
  /// @brief Reads the device unique id of the first member a members-created
  /// notification names, `body.data.members[0].deviceUniqueId`.
  ///
  /// @return The 32 bytes, decoded from hex.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type or names no member with a device id.
  /// @retval bad_reply::InvalidDeviceUid The id is not 64 hex digits.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto member_device_uid() const
      -> Expected<std::array<std::byte, psn_device_uid_size>, DeviceUidFault>;
  /// @brief Reads the session's new customData1 from a customData1-updated
  /// notification, `body.data.customData1`.
  ///
  /// The field is 32 characters of base64 whose decoded text is itself base64
  /// of 16 to 20 bytes; the first 16 are returned.
  ///
  /// @return The first psn_custom_data_size bytes.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type or lacks the field.
  /// @retval bad_reply::InvalidCustomData1 The field is not as
  ///     described.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto custom_data1() const
      -> Expected<std::array<std::byte, psn_custom_data_size>,
                  CustomData1Fault>;
  /// @brief Reads the payload of a session-message notification,
  /// `body.data.sessionMessage.payload`.
  ///
  /// @return The payload text; secret.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type or lacks the payload.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto session_message_payload() const
      -> Expected<SecretString, MessageFault>;
  /// @brief Reads the message a lounge-group notification announces, from
  /// `from.onlineId` and `body.extendedData.data.value`.
  ///
  /// A group changes for other reasons too, such as a member reading it;
  /// only a change that names a message (`messageUid`) is a message.
  ///
  /// @return The message.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type, names no message or no sender.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto lounge_message() const
      -> Expected<LoungeMessage, MessageFault>;
  /// @brief Reads the invitation an invitation push announces, from
  /// `body.data.customProperties.from.onlineId` and `body.data.sessionId`.
  ///
  /// @return The invitation.
  /// @retval bad_reply::InvalidSchema The notification is of another
  ///     type, or names no session or no inviter.
  /// @retval MessageFault The body's parse failure.
  [[nodiscard]] auto session_invitation() const
      -> Expected<SessionInvitation, MessageFault>;
  /// @brief Tells whether a notification concerns a session, by
  /// `body.data.sessionId`.
  ///
  /// @param session_id The session.
  /// @param require_id Whether a notification naming no session counts as not
  ///     belonging.
  /// @return True when it names `session_id`, or names none and `require_id`
  ///     is false; false for an unreadable body.
  auto belongs_to_session(std::string_view session_id, bool require_id) const
      -> bool;

 private:
  /// @brief The length of the `customData1` text, in characters.
  static constexpr std::size_t custom_data1_text_length = 32;
  /// @brief The most bytes the doubly decoded customData1 may hold.
  static constexpr std::size_t custom_data1_maximum_bytes = 20;
  /// @brief The `dataType` of remote_play_session_created.
  static constexpr std::string_view session_created_type =
      "psn:sessionManager:sys:remotePlaySession:created";
  /// @brief The `dataType` of members_created.
  static constexpr std::string_view members_created_type =
      "psn:sessionManager:sys:rps:members:created";
  /// @brief The `dataType` of custom_data1_updated.
  static constexpr std::string_view custom_data1_updated_type =
      "psn:sessionManager:sys:rps:customData1:updated";
  /// @brief The `dataType` of session_message_created.
  static constexpr std::string_view session_message_created_type =
      "psn:sessionManager:sys:rps:sessionMessage:created";
  /// @brief The `dataType` of members_deleted.
  static constexpr std::string_view members_deleted_type =
      "psn:sessionManager:sys:rps:members:deleted";
  /// @brief The `dataType` of remote_play_session_deleted.
  static constexpr std::string_view session_deleted_type =
      "psn:sessionManager:sys:remotePlaySession:deleted";

  /// @brief The `dataType` of invitation_created.
  static constexpr std::string_view invitation_created_type =
      "psn:sessionManager:sys:v2:ps:invitations:created";
  /// @brief The `dataType` of lounge_group_changed.
  static constexpr std::string_view lounge_group_type =
      "np:service:gamingLoungeGroups";

  /// @brief The `dataType` every notification carries.
  struct Envelope;
  /// @brief The `to.onlineId` of a session-created notification.
  struct Addressed;
  /// @brief The members of a members-created notification.
  struct MembersData;
  /// @brief The customData1 of a customData1-updated notification.
  struct CustomData;
  /// @brief The payload of a session-message notification.
  struct SessionMessageData;
  /// @brief The fields of a lounge-group notification.
  struct LoungeData;
  /// @brief The fields of an invitation notification.
  struct InvitationData;
  /// @brief The session id a notification names.
  struct SessionData;

  /// @brief Finds a notification's type.
  ///
  /// @param canonical The canonical body, or its parse failure.
  /// @return The type; unknown for a failure, no `dataType` or an unknown
  ///     one.
  static auto classify(
      const Expected<SecretString, json::ParseError>& canonical)
      -> NotificationType;
  /// @brief Decodes the canonical body as one type's message.
  ///
  /// @tparam Message The message's shape.
  /// @param expected The type the notification must have.
  /// @return The message.
  /// @retval bad_reply::InvalidSchema The type differs or the body does
  ///     not decode.
  /// @retval MessageFault The body's parse failure.
  template <class Message>
  [[nodiscard]] auto typed(NotificationType expected) const
      -> Expected<Message, MessageFault>;

  /// @brief Keeps a body and its canonical form, and classifies it.
  ///
  /// @param body The body as received.
  /// @param canonical Its canonical form, or why there is none.
  Notification(SecretString body,
               Expected<SecretString, json::ParseError> canonical) noexcept;

  /// @brief The body as received.
  SecretString body_;
  /// @brief The body as canonical JSON, or why it could not be read.
  Expected<SecretString, json::ParseError> canonical_;
  /// @brief The type, found once.
  NotificationType type_;
};

/// @brief The WebSocket request that opens the PSN push channel.
///
/// `wss://<fqdn>/np/pushNotification`, subprotocol `np-pushpacket`, the token
/// as a bearer token and the `X-PSN-*` headers of the app it is opened as
/// (protocol 2.1, keep-alive status type 3); system TLS trust; a 30 s connect
/// timeout;
/// pings every 5 s from the start, a pong due within 5 s; messages up to
/// 64 KiB. An immutable value; it holds the token, so it is secret.
class PushChannelRequest {
 public:
  /// @brief Checks a push server host before it is put in a URL.
  ///
  /// @param host The host.
  /// @return True when it is 1 to 253 visible ASCII bytes with no `/`, `?`,
  ///     `#`, `@` or `\`.
  static auto valid_fqdn(std::string_view host) -> bool;

  /// @brief Builds the request.
  ///
  /// @param token The Remote Play token for PushApp::remote_play, the app
  ///     token for PushApp::mobile.
  /// @param fqdn The push server, from Push::server_address().
  /// @param app The app the channel is opened as.
  /// @return The request.
  /// @retval bad_request::InvalidFqdn The host is not valid_fqdn().
  [[nodiscard]] static auto create(const auth::BearerToken& token,
                                   std::string_view fqdn,
                                   PushApp app = PushApp::remote_play)
      -> Expected<PushChannelRequest, Fault<bad_request::InvalidFqdn>>;

  /// @brief Reads the request to hand to the WebSocket provider.
  ///
  /// @return The request; valid while this object lives.
  auto request() const noexcept -> const platform::WebSocketRequest& {
    return request_;
  }

 private:
  /// @brief The longest host accepted, in bytes.
  static constexpr std::size_t fqdn_max_bytes = 253;
  /// @brief The channel's path on the push server.
  static constexpr std::string_view channel_path = "/np/pushNotification";
  /// @brief How long the connection may take.
  static constexpr std::chrono::seconds push_connect_timeout{30};
  /// @brief How often the client pings.
  static constexpr std::chrono::seconds push_ping_interval{5};
  /// @brief How long a pong may take before the channel counts as dead.
  static constexpr std::chrono::seconds push_pong_timeout{5};
  /// @brief The largest message accepted, in bytes.
  static constexpr std::size_t push_max_message_bytes = 64U * 1024U;
  /// @brief The `X-PSN-APP-TYPE` of a mobile client.
  static constexpr std::string_view mobile_app_type = "MOBILE_APP.PSAPP";
  /// @brief The client version sent in `X-PSN-APP-VER`.
  static constexpr std::string_view mobile_app_version = "26.10.0";
  /// @brief The operating system sent in `X-PSN-OS-VER` for a mobile client.
  static constexpr std::string_view mobile_os_version = "Android/14";

  /// @brief Wraps a built request.
  ///
  /// @param request The request.
  explicit PushChannelRequest(platform::WebSocketRequest request) noexcept;

  /// @brief The request.
  platform::WebSocketRequest request_;
};

/// @brief Asks PSN which push server the user's channel should connect to.
///
/// Holds a reference to the HTTP client, which must outlive it, and the Remote
/// Play token. One request per call; not for concurrent use.
class Push {
 public:
  /// @brief The failure of server_address().
  using ServerAddressError =
      Error<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
            bad_reply::InvalidSchema, bad_reply::InvalidFqdn>;
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The Remote Play token.
  Push(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Finds the push server.
  ///
  /// GET
  /// `https://mobile-pushcl.np.communication.playstation.net/np/serveraddr?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3`
  /// with the token the channel will open with; the reply's `fqdn` is read in
  /// PS's wire dialect. The reply is kept even if the stop token fires
  /// mid-request. Blocks for one round trip.
  ///
  /// @return The push server's host name.
  /// @retval Error The request failed or answered with an error status.
  /// @retval bad_reply::InvalidSchema The reply has no `fqdn`.
  /// @retval bad_reply::InvalidFqdn The `fqdn` is not valid_fqdn().
  /// @retval bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson
  ///     The reply could not be parsed.
  [[nodiscard]] auto server_address()
      -> Expected<std::string, ServerAddressError>;

 private:
  /// @brief The server address reply's `fqdn`.
  struct ServerAddress;

  /// @brief The server address endpoint.
  static constexpr std::string_view server_address_url =
      "https://mobile-pushcl.np.communication.playstation.net/np/serveraddr";

  /// @brief The server address request, logged with its query and kept after
  /// a stop.
  static constexpr net::http::Endpoint server_address_endpoint{
      .label = "psn.push.server_address",
      .log_query = true,
      .keep_reply_after_stop = true,
  };

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The Remote Play token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
