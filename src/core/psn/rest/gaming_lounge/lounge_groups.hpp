/// @file
/// @brief Declares LoungeGroups, the client that reads and writes PSN's
/// message groups, and the plain rows it reads: the
/// account's conversations and a conversation's messages.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>
#include "src/core/psn/auth/credentials.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief One message of a conversation. Plain data; text fields are empty
/// when PS omitted them.
struct ChatMessageRow {
  /// @brief The `messageUid`.
  std::string message_uid;
  /// @brief The `messageType`: 1 for text, 3 for text with an image, 8 for
  /// text with a store link; other values are stickers, voice, events and the
  /// like, and 2006 marks the start of the conversation.
  std::int64_t message_type{};
  /// @brief The `body`, the message's text.
  std::string body;
  /// @brief The `createdTimestamp`, in milliseconds since the epoch; 0 when
  /// PS gave none.
  std::int64_t created_ms{};
  /// @brief The `sender.accountId` in decimal.
  std::string sender_account_id;
  /// @brief The `sender.onlineId`.
  std::string sender_online_id;
};

/// @brief One member of a conversation. Plain data.
struct ChatMemberRow {
  /// @brief The `accountId` in decimal.
  std::string account_id;
  /// @brief The `onlineId`.
  std::string online_id;
};

/// @brief One of the account's conversations. Plain data; text fields are
/// empty when PS omitted them.
struct ChatGroupRow {
  /// @brief The `groupId`.
  std::string group_id;
  /// @brief The `groupType`: 0 for a direct conversation, 1 for a group.
  std::int64_t group_type{};
  /// @brief The `groupName.value`, set only for named groups.
  std::string name;
  /// @brief The `members`, the user included.
  std::vector<ChatMemberRow> members;
  /// @brief The `mainThread.threadId`, which messages are read and sent in.
  std::string thread_id;
  /// @brief The `mainThread.latestMessage`, when PS gave one.
  std::optional<ChatMessageRow> latest;
  /// @brief The `modifiedTimestamp`, in milliseconds since the epoch; 0 when
  /// PS gave none.
  std::int64_t modified_ms{};
  /// @brief Whether `mainThread.existsUnreadMessage` or `existsNewArrival`
  /// is true.
  bool unread{};
  /// @brief The `isFavorite`.
  bool favorite{};
};

/// @brief One page of the account's conversations.
struct ChatGroupsPage {
  /// @brief The conversations PS listed, in reply order.
  std::vector<ChatGroupRow> groups;
  /// @brief How many entries the reply held, readable or not, to tell a short
  /// page from a full one.
  std::size_t received{};
};

/// @brief Reads and writes the account's message groups with the app token.
///
/// Every address is under
/// `https://m.np.playstation.com/api/gamingLoungeGroups`, and every request
/// sends `Accept-Language`. Holds a reference to the HTTP client, which must
/// outlive it, and the token. Each call blocks for one round trip. Not for
/// concurrent use.
class LoungeGroups {
 public:
  /// @brief The failure of mark_read().
  using MarkReadError = Error<bad_request::InvalidMessage>;
  /// @brief The failure of send().
  using SendError = Error<bad_request::InvalidMessage, bad_reply::InvalidJson,
                          bad_reply::InvalidSchema>;
  /// @brief The failure of messages().
  using MessagesError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of groups().
  using GroupsError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The most conversations one page asks for.
  static constexpr std::size_t groups_page_size = 200;
  /// @brief The `messageType` of a text message.
  static constexpr std::int64_t text_message = 1;

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  /// @param language The locale to send, such as `en-GB`.
  LoungeGroups(net::http::Client& client, auth::BearerToken token,
               std::string language) noexcept;

  /// @brief Reads one page of the account's conversations.
  ///
  /// GET `/v1/members/me/groups?includeFields=groupName,groupIcon,members,
  /// mainThread,joinedTimestamp,modifiedTimestamp,totalGroupCount,
  /// isFavorite,existsNewArrival,notificationSetting&favoriteFilter=<filter>
  /// &limit=200&offset=<offset>`; unreadable entries are skipped, and a
  /// reply without `groups` is an empty page.
  ///
  /// @param offset Index of the first conversation.
  /// @param favorite True for the favourite conversations
  ///     (`favoriteFilter=favorite`), false for the rest (`notFavorite`).
  /// @return The page.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto groups(std::size_t offset, bool favorite)
      -> Expected<ChatGroupsPage, GroupsError>;
  /// @brief Reads a conversation's newest messages, or the ones before a
  /// message.
  ///
  /// GET `/v1/members/me/groups/<group>/threads/<thread>/messages?
  /// sharedTypes=profile,party&includeFields=sharedUrlInfo`, adding
  /// `before=<message>&limit=50` for an older page; entries without a body
  /// are kept so the start-of-conversation marker is seen, unreadable ones
  /// are skipped, and a reply without `messages` holds none.
  ///
  /// @param group_id The `groupId`.
  /// @param thread_id The conversation's `threadId`.
  /// @param before The `messageUid` to read before; empty for the newest.
  /// @return The messages, in reply order.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto messages(std::string_view group_id,
                              std::string_view thread_id,
                              std::string_view before)
      -> Expected<std::vector<ChatMessageRow>, MessagesError>;
  /// @brief Sends a text message.
  ///
  /// POST `/v1/groups/<group>/threads/<thread>/messages` with
  /// `{"messageType":1,"body":<text>}`.
  ///
  /// @param group_id The `groupId`.
  /// @param thread_id The conversation's `threadId`.
  /// @param text The text.
  /// @return The new message's `messageUid`; empty when PS named none.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto send(std::string_view group_id, std::string_view thread_id,
                          std::string_view text)
      -> Expected<std::string, SendError>;
  /// @brief Marks a conversation read up to a message.
  ///
  /// PATCH `/v1/members/me/groups/<group>/threads/<thread>` with
  /// `{"readMessageUid":<message>}`.
  ///
  /// @param group_id The `groupId`.
  /// @param thread_id The conversation's `threadId`.
  /// @param message_uid The newest message read.
  /// @return Nothing once PS accepted it.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto mark_read(std::string_view group_id,
                               std::string_view thread_id,
                               std::string_view message_uid)
      -> Expected<void, MarkReadError>;

 private:
  /// @brief The service's base address.
  static constexpr std::string_view base =
      "https://m.np.playstation.com/api/gamingLoungeGroups";
  /// @brief The fields the conversation list asks for.
  static constexpr std::string_view group_fields =
      "groupName,groupIcon,members,mainThread,joinedTimestamp,"
      "modifiedTimestamp,totalGroupCount,isFavorite,existsNewArrival,"
      "notificationSetting";
  /// @brief How many older messages one page asks for.
  static constexpr std::size_t older_page_size = 50;
  /// @brief The content type writes send.
  static constexpr std::string_view json_content_type =
      "application/json; charset=utf-8";

  /// @brief The conversation list, logged with its query.
  static constexpr net::http::Endpoint groups_endpoint{
      .label = "psn.gaming_lounge.groups",
      .log_query = true,
  };
  /// @brief A conversation's messages.
  static constexpr net::http::Endpoint messages_endpoint{
      .label = "psn.gaming_lounge.messages",
  };
  /// @brief A sent message.
  static constexpr net::http::Endpoint send_endpoint{
      .label = "psn.gaming_lounge.send",
      .method = net::http::Method::post,
  };
  /// @brief The read marker.
  static constexpr net::http::Endpoint mark_read_endpoint{
      .label = "psn.gaming_lounge.mark_read",
      .method = net::http::Method::patch,
  };

  /// @brief The conversation list's reply.
  struct GroupsReply;
  /// @brief A conversation's messages reply.
  struct MessagesReply;
  /// @brief One message as PS writes it.
  struct MessageWire;
  /// @brief The sent message's body.
  struct SendBody;
  /// @brief The sent message's reply.
  struct SendReply;
  /// @brief The read marker's body.
  struct ReadBody;

  /// @brief Turns a message as PS writes it into a row.
  ///
  /// @param wire The message, consumed.
  /// @return The row.
  static auto row_of(MessageWire wire) -> ChatMessageRow;
  /// @brief Builds the address of a conversation's thread.
  ///
  /// @param owner `/v1/members/me/groups/` or `/v1/groups/`.
  /// @param group_id The `groupId`.
  /// @param thread_id The `threadId`.
  /// @return `<base><owner><group>/threads/<thread>`, the ids
  ///     percent-encoded.
  static auto thread_address(std::string_view owner, std::string_view group_id,
                             std::string_view thread_id) -> std::string;
  /// @brief Signs a request and adds the headers every call sends.
  ///
  /// @param request The request.
  void prepare(net::http::Request& request) const;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
  /// @brief The locale sent as `Accept-Language`.
  std::string language_;
};

}  // namespace libchiaking::psn::rest
