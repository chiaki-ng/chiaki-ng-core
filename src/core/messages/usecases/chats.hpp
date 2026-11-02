/// @file
/// @brief Declares the Chats use case: listing a user's message
/// conversations, reading and marking one, and sending text in it.
#pragma once

#include "src/core/psn/rest/gaming_lounge/lounge_groups.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/messages/conversation.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::messages::usecases {

/// @brief Talks to PSN for Messages and turns its rows into the public
/// types.
///
/// Holds references only; may be used from several threads at once.
/// Everything it refers to must outlive it.
class Chats {
 public:
  /// @brief The `messageType`s that carry text the client shows: text, text
  /// with an image and text with a store link.
  static constexpr std::array<std::int64_t, 3> text_types{1, 3, 8};
  /// @brief The `messageType` that marks the start of a conversation.
  static constexpr std::int64_t start_type = 2006;

  /// @brief Prepares the use case.
  ///
  /// @param http Sends every request.
  /// @param clock The monotonic clock requests are timed by.
  /// @param wall_clock Device time.
  /// @param log Receives log lines; may be null.
  Chats(platform::HttpProvider& http, const platform::ClockProvider& clock,
        const platform::ClockProvider& wall_clock,
        platform::DiagnosticsProvider* log) noexcept;

  /// @brief Lists the user's conversations, as Messages::conversations.
  ///
  /// @param user The signed-in user.
  /// @param stop Cancels the requests.
  /// @return The conversations, most recently changed first.
  /// @retval users::Unanswered Why PSN did not answer.
  [[nodiscard]] auto conversations(users::PsUser& user,
                                   const platform::StopToken& stop)
      -> Expected<std::vector<Conversation>, users::Unanswered>;
  /// @brief Reads a conversation's messages, as Messages::thread.
  ///
  /// @param user The signed-in user.
  /// @param group_id The group.
  /// @param thread_id The thread.
  /// @param before The message to read before; empty for the newest.
  /// @param stop Cancels the request.
  /// @return The messages, oldest first.
  /// @retval users::Unanswered Why PSN did not answer, or rejected for an
  ///     empty id.
  [[nodiscard]] auto thread(users::PsUser& user, std::string_view group_id,
                            std::string_view thread_id, std::string_view before,
                            const platform::StopToken& stop)
      -> Expected<ChatThread, users::Unanswered>;
  /// @brief Sends text, as Messages::send.
  ///
  /// @param user The signed-in user.
  /// @param group_id The group.
  /// @param thread_id The thread.
  /// @param text The text.
  /// @param stop Cancels the request.
  /// @return The message as sent.
  /// @retval users::Unanswered Why it was not sent.
  [[nodiscard]] auto send(users::PsUser& user, std::string_view group_id,
                          std::string_view thread_id, std::string_view text,
                          const platform::StopToken& stop)
      -> Expected<ChatMessage, users::Unanswered>;
  /// @brief Marks a conversation read, as Messages::mark_read.
  ///
  /// @param user The signed-in user.
  /// @param group_id The group.
  /// @param thread_id The thread.
  /// @param message_id The newest message shown.
  /// @param stop Cancels the request.
  /// @return Nothing once PSN accepted it.
  /// @retval users::Unanswered Why PSN did not accept it.
  [[nodiscard]] auto mark_read(users::PsUser& user, std::string_view group_id,
                               std::string_view thread_id,
                               std::string_view message_id,
                               const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;

  /// @brief Turns a message row into a message.
  ///
  /// @param row The row.
  /// @param account_id The user's account id in decimal.
  /// @return The message: text for a text type with a body, other
  ///     otherwise.
  static auto message_of(const psn::rest::ChatMessageRow& row,
                         std::string_view account_id) -> ChatMessage;
  /// @brief Turns a conversation row into a conversation.
  ///
  /// @param row The row.
  /// @param account_id The user's account id in decimal, left out of the
  ///     members.
  /// @return The conversation.
  static auto conversation_of(const psn::rest::ChatGroupRow& row,
                              std::string_view account_id) -> Conversation;
  /// @brief Turns a page of message rows into a thread.
  ///
  /// @param rows The rows, in reply order.
  /// @param account_id The user's account id in decimal.
  /// @return The messages without the start marker, oldest first, complete
  ///     when the marker was among them.
  static auto thread_of(std::span<const psn::rest::ChatMessageRow> rows,
                        std::string_view account_id) -> ChatThread;

 private:
  /// @brief The locale PS is asked to write in.
  ///
  /// @param user The user.
  /// @return The account's locale, or Messages::default_locale.
  static auto language_of(const users::PsUser& user) -> std::string;
  /// @brief A time PS wrote in milliseconds since the epoch.
  ///
  /// @param milliseconds The time; 0 for none.
  /// @return The time.
  static auto time_of(std::int64_t milliseconds) noexcept -> MessageTime;

  /// @brief Sends every request.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Device time.
  const platform::ClockProvider& wall_clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::messages::usecases
