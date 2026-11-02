#include "src/core/messages/usecases/chats.hpp"

#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/message_calls.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <libchiaking/detail/messages/messages.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <algorithm>
#include <utility>

namespace libchiaking::messages::usecases {

Chats::Chats(platform::HttpProvider& http, const platform::ClockProvider& clock,
             const platform::ClockProvider& wall_clock,
             platform::DiagnosticsProvider* log) noexcept
    : http_(http), clock_(clock), wall_clock_(wall_clock), log_(log) {}

auto Chats::conversations(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<Conversation>, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  users::MessageCalls calls{user};
  const auto language = language_of(user);
  const auto account_id = user.account_id().decimal();
  std::vector<Conversation> found;
  for (const bool favorite : {true, false}) {
    for (std::size_t page = 0; page < Messages::most_pages; ++page) {
      auto listed = calls.chat_groups(
          http, language, page * psn::rest::LoungeGroups::groups_page_size,
          favorite);
      if (!listed) return unexpected(listed.error());
      for (const auto& row : listed->groups) {
        if (std::ranges::contains(found, row.group_id, &Conversation::group_id))
          continue;
        found.push_back(conversation_of(row, account_id));
      }
      if (listed->received < psn::rest::LoungeGroups::groups_page_size) break;
    }
  }
  std::ranges::stable_sort(found, std::ranges::greater{},
                           &Conversation::updated);
  http.log(platform::LogSeverity::info,
           "[MESSAGES] " + std::to_string(found.size()) + " conversations");
  return found;
}

auto Chats::thread(users::PsUser& user, std::string_view group_id,
                   std::string_view thread_id, std::string_view before,
                   const platform::StopToken& stop)
    -> Expected<ChatThread, users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  if (text::trim(group_id).empty() || text::trim(thread_id).empty())
    return unexpected(users::Unanswered{Reason::rejected});
  net::http::Client http{http_, clock_, stop, log_};
  auto rows = users::MessageCalls{user}.chat_messages(
      http, language_of(user), text::trim(group_id), text::trim(thread_id),
      text::trim(before));
  if (!rows) return unexpected(rows.error());
  return thread_of(rows.value(), user.account_id().decimal());
}

auto Chats::send(users::PsUser& user, std::string_view group_id,
                 std::string_view thread_id, std::string_view text,
                 const platform::StopToken& stop)
    -> Expected<ChatMessage, users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  const auto said = text::trim(text);
  if (said.empty() || !text::utf8::valid(said) ||
      text::utf8::utf16_length(said) > Messages::longest_text ||
      text::trim(group_id).empty() || text::trim(thread_id).empty())
    return unexpected(users::Unanswered{Reason::rejected});
  net::http::Client http{http_, clock_, stop, log_};
  auto sent = users::MessageCalls{user}.send_chat_message(
      http, language_of(user), text::trim(group_id), text::trim(thread_id),
      said);
  if (!sent) return unexpected(sent.error());
  http.log(platform::LogSeverity::info, "[MESSAGES] message sent");
  return ChatMessage{
      .id = std::move(sent.value()),
      .kind = ChatMessage::Kind::text,
      .text = std::string{said},
      .sender = user.online_id(),
      .sender_account_id = user.account_id().decimal(),
      .from_user = true,
      .sent = MessageTime{std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::microseconds{
              static_cast<std::int64_t>(wall_clock_.now_microseconds())})}};
}

auto Chats::mark_read(users::PsUser& user, std::string_view group_id,
                      std::string_view thread_id, std::string_view message_id,
                      const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  if (text::trim(group_id).empty() || text::trim(thread_id).empty() ||
      text::trim(message_id).empty())
    return unexpected(users::Unanswered{Reason::rejected});
  net::http::Client http{http_, clock_, stop, log_};
  return users::MessageCalls{user}.mark_chat_read(
      http, language_of(user), text::trim(group_id), text::trim(thread_id),
      text::trim(message_id));
}

auto Chats::message_of(const psn::rest::ChatMessageRow& row,
                       std::string_view account_id) -> ChatMessage {
  const bool shown = std::ranges::contains(text_types, row.message_type) &&
                     !text::trim(row.body).empty();
  return ChatMessage{
      .id = row.message_uid,
      .kind = shown ? ChatMessage::Kind::text : ChatMessage::Kind::other,
      .text = shown ? row.body : std::string{},
      .sender = row.sender_online_id,
      .sender_account_id = row.sender_account_id,
      .from_user =
          !row.sender_account_id.empty() && row.sender_account_id == account_id,
      .sent = time_of(row.created_ms)};
}

auto Chats::conversation_of(const psn::rest::ChatGroupRow& row,
                            std::string_view account_id) -> Conversation {
  Conversation conversation{.group_id = row.group_id,
                            .thread_id = row.thread_id,
                            .group = row.group_type != 0,
                            .name = std::string{text::trim(row.name)},
                            .updated = time_of(row.modified_ms),
                            .unread = row.unread,
                            .favorite = row.favorite};
  for (const auto& member : row.members)
    if (member.account_id != account_id)
      conversation.members.push_back({member.account_id, member.online_id});
  if (row.latest) conversation.latest = message_of(*row.latest, account_id);
  return conversation;
}

auto Chats::thread_of(std::span<const psn::rest::ChatMessageRow> rows,
                      std::string_view account_id) -> ChatThread {
  ChatThread thread;
  for (const auto& row : rows) {
    if (row.message_type == start_type) {
      thread.complete = true;
      continue;
    }
    thread.messages.push_back(message_of(row, account_id));
  }
  std::ranges::stable_sort(thread.messages, std::ranges::less{},
                           &ChatMessage::sent);
  return thread;
}

auto Chats::language_of(const users::PsUser& user) -> std::string {
  return user.account_context().locale().value_or(
      std::string{Messages::default_locale});
}

auto Chats::time_of(std::int64_t milliseconds) noexcept -> MessageTime {
  return MessageTime{std::chrono::milliseconds{milliseconds}};
}

}  // namespace libchiaking::messages::usecases
