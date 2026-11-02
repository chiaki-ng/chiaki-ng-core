#include "src/core/messages/usecases/chats.hpp"

#include <libchiaking/detail/messages/messages.hpp>

namespace libchiaking::messages {

Messages::Messages(platform::HttpProvider& http,
                   const platform::ClockProvider& clock,
                   const platform::ClockProvider& wall_clock,
                   platform::DiagnosticsProvider* diagnostics) noexcept
    : http_(http), clock_(clock), wall_clock_(wall_clock), log_(diagnostics) {}

auto Messages::conversations(users::PsUser& user,
                             const platform::StopToken& stop)
    -> Expected<std::vector<Conversation>, users::Unanswered> {
  return usecases::Chats{http_, clock_, wall_clock_, log_}.conversations(user,
                                                                       stop);
}

auto Messages::thread(users::PsUser& user, std::string_view group_id,
                      std::string_view thread_id, std::string_view before,
                      const platform::StopToken& stop)
    -> Expected<ChatThread, users::Unanswered> {
  return usecases::Chats{http_, clock_, wall_clock_, log_}.thread(
      user, group_id, thread_id, before, stop);
}

auto Messages::send(users::PsUser& user, std::string_view group_id,
                    std::string_view thread_id, std::string_view text,
                    const platform::StopToken& stop)
    -> Expected<ChatMessage, users::Unanswered> {
  return usecases::Chats{http_, clock_, wall_clock_, log_}.send(
      user, group_id, thread_id, text, stop);
}

auto Messages::mark_read(users::PsUser& user, std::string_view group_id,
                         std::string_view thread_id,
                         std::string_view message_id,
                         const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  return usecases::Chats{http_, clock_, wall_clock_, log_}.mark_read(
      user, group_id, thread_id, message_id, stop);
}

}  // namespace libchiaking::messages
