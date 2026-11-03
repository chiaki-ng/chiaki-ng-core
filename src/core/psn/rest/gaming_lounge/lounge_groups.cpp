#include "src/core/psn/rest/gaming_lounge/lounge_groups.hpp"

#include "src/core/net/transport/http/percent_encoding.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct LoungeGroups::MessageWire {
  struct Sender {
    using keys = ps::CamelCaseKeys;
    ps::DecimalText account_id;
    ps::Tolerant<std::string> online_id;
  };
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::string> message_uid;
  ps::Tolerant<std::int64_t> message_type;
  ps::Tolerant<std::string> body;
  ps::DecimalText created_timestamp;
  ps::Tolerant<Sender> sender;
};

struct LoungeGroups::GroupsReply {
  struct Name {
    ps::Tolerant<std::string> value;
  };
  struct Member {
    using keys = ps::CamelCaseKeys;
    ps::DecimalText account_id;
    ps::Tolerant<std::string> online_id;
  };
  struct Thread {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> thread_id;
    ps::Tolerant<MessageWire> latest_message;
    ps::Tolerant<bool> exists_unread_message;
  };
  struct Group {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> group_id;
    ps::Tolerant<std::int64_t> group_type;
    ps::Tolerant<Name> group_name;
    ps::Tolerant<std::vector<ps::Tolerant<Member>>> members;
    ps::Tolerant<Thread> main_thread;
    ps::DecimalText modified_timestamp;
    ps::Tolerant<bool> exists_new_arrival;
    ps::Tolerant<bool> is_favorite;
  };
  std::optional<std::vector<ps::Tolerant<Group>>> groups;
};

struct LoungeGroups::MessagesReply {
  std::optional<std::vector<ps::Tolerant<MessageWire>>> messages;
};

struct LoungeGroups::SendBody {
  using keys = ps::CamelCaseKeys;
  std::int64_t message_type{};
  std::string_view body;
};

struct LoungeGroups::SendReply {
  using keys = ps::CamelCaseKeys;
  ps::Tolerant<std::string> message_uid;
};

struct LoungeGroups::ReadBody {
  using keys = ps::CamelCaseKeys;
  std::string_view read_message_uid;
};

LoungeGroups::LoungeGroups(net::http::Client& client, auth::BearerToken token,
                           std::string language) noexcept
    : client_(client),
      token_(std::move(token)),
      language_(std::move(language)) {}

auto LoungeGroups::row_of(MessageWire wire) -> ChatMessageRow {
  ChatMessageRow row{
      .message_uid =
          std::string{text::trim(std::move(wire.message_uid).value_or(""))},
      .message_type = wire.message_type.value_or(0),
      .body = std::move(wire.body).value_or(""),
      .created_ms = wire.created_timestamp.integer().value_or(0)};
  if (wire.sender) {
    row.sender_account_id = wire.sender->account_id.trimmed();
    row.sender_online_id = std::move(wire.sender->online_id).value_or("");
  }
  return row;
}

auto LoungeGroups::thread_address(std::string_view owner,
                                  std::string_view group_id,
                                  std::string_view thread_id) -> std::string {
  std::string address{base};
  address += owner;
  address += net::http::PercentEncoding::component(group_id);
  address += "/threads/";
  address += net::http::PercentEncoding::component(thread_id);
  return address;
}

void LoungeGroups::prepare(net::http::Request& request) const {
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language_);
}

auto LoungeGroups::groups(std::size_t offset, bool favorite)
    -> Expected<ChatGroupsPage, GroupsError> {
  std::string address{base};
  address += "/v1/members/me/groups";
  net::http::Query url{address};
  url.add_verbatim("includeFields", group_fields)
      .add_verbatim("favoriteFilter", favorite ? "favorite" : "notFavorite")
      .add_verbatim("limit", std::to_string(groups_page_size))
      .add_verbatim("offset", std::to_string(offset));
  net::http::Request request{groups_endpoint, std::move(url)};
  prepare(request);
  auto reply = GroupsError::checked_reply<GroupsReply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  ChatGroupsPage page;
  if (!reply->groups) return page;
  page.received = reply->groups->size();
  for (auto& slot : *reply->groups) {
    if (!slot) continue;
    auto& group = *slot;
    ChatGroupRow row{
        .group_id =
            std::string{text::trim(std::move(group.group_id).value_or(""))},
        .group_type = group.group_type.value_or(0),
        .modified_ms = group.modified_timestamp.integer().value_or(0),
        .unread = group.exists_new_arrival.value_or(false),
        .favorite = group.is_favorite.value_or(false)};
    if (row.group_id.empty()) continue;
    if (group.group_name)
      row.name = std::move(group.group_name->value).value_or("");
    if (group.members)
      for (auto& member : *group.members)
        if (member)
          row.members.push_back({member->account_id.trimmed(),
                                 std::move(member->online_id).value_or("")});
    if (group.main_thread) {
      auto& thread = *group.main_thread;
      row.thread_id =
          std::string{text::trim(std::move(thread.thread_id).value_or(""))};
      if (thread.latest_message)
        row.latest = row_of(std::move(*thread.latest_message));
      row.unread = row.unread || thread.exists_unread_message.value_or(false);
    }
    page.groups.push_back(std::move(row));
  }
  return page;
}

auto LoungeGroups::messages(std::string_view group_id,
                            std::string_view thread_id, std::string_view before)
    -> Expected<std::vector<ChatMessageRow>, MessagesError> {
  net::http::Query url{
      thread_address("/v1/members/me/groups/", group_id, thread_id) +
      "/messages"};
  url.add_verbatim("sharedTypes", "profile,party")
      .add_verbatim("includeFields", "sharedUrlInfo");
  if (!before.empty())
    url.add("before", before)
        .add_verbatim("limit", std::to_string(older_page_size));
  net::http::Request request{messages_endpoint, std::move(url)};
  prepare(request);
  auto reply = MessagesError::checked_reply<MessagesReply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  std::vector<ChatMessageRow> rows;
  if (!reply->messages) return rows;
  rows.reserve(reply->messages->size());
  for (auto& slot : *reply->messages)
    if (slot) rows.push_back(row_of(std::move(*slot)));
  return rows;
}

auto LoungeGroups::send(std::string_view group_id, std::string_view thread_id,
                        std::string_view text)
    -> Expected<std::string, SendError> {
  auto body = ps::write(SendBody{.message_type = text_message, .body = text});
  if (!body) return unexpected(SendError{bad_request::InvalidMessage{}});
  net::http::Request request{
      send_endpoint,
      net::http::Query{thread_address("/v1/groups/", group_id, thread_id) +
                       "/messages"}};
  prepare(request);
  request.body(json_content_type, SecretBytes{body->bytes()});
  auto reply = SendError::checked_reply<SendReply>(
      client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  return std::string{text::trim(std::move(reply->message_uid).value_or(""))};
}

auto LoungeGroups::mark_read(std::string_view group_id,
                             std::string_view thread_id,
                             std::string_view message_uid)
    -> Expected<void, MarkReadError> {
  auto body = ps::write(ReadBody{message_uid});
  if (!body) return unexpected(MarkReadError{bad_request::InvalidMessage{}});
  net::http::Request request{
      mark_read_endpoint, net::http::Query{thread_address(
                              "/v1/members/me/groups/", group_id, thread_id)}};
  prepare(request);
  request.body(json_content_type, SecretBytes{body->bytes()});
  auto sent =
      MarkReadError::checked(client_.send(std::move(request)), token_.view());
  if (!sent) return unexpected(std::move(sent.error()));
  return {};
}

}  // namespace libchiaking::psn::rest
