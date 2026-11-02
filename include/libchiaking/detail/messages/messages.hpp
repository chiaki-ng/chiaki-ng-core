/// @file
/// @brief Declares Messages, the service that reads a signed-in user's PSN
/// message conversations and sends text in them.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/messages/conversation.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::messages {

/// @brief Reads a signed-in user's message conversations from PSN and sends
/// text in them.
///
/// Every call is signed with the user's app token; when PSN refuses the
/// token it is renewed once and the request sent again, as every other
/// app-token call in the library does. Each call blocks the calling thread
/// while it talks to PSN; the stop token cancels it. Nothing is kept between
/// calls. Holds references only; may be used from several threads at once.
///
/// A user who is signed out is turned away before any request is made.
/// @ingroup api_messages
class Messages {
 public:
  /// @brief The most pages of conversations read in one listing.
  static constexpr std::size_t most_pages = 5;
  /// @brief The locale sent when the account states none.
  static constexpr std::string_view default_locale = "en-US";
  /// @brief The longest text sent, measured in UTF-16 code units.
  static constexpr std::size_t longest_text = 512;

  /// @brief Not copyable.
  Messages(const Messages&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Messages&) -> Messages& = delete;

  /// @brief Lists the user's conversations, most recently changed first.
  ///
  /// Reads the favourite conversations and then the others, a page at a
  /// time up to most_pages pages each.
  ///
  /// @param user The signed-in user.
  /// @param stop Cancels the requests.
  /// @return The conversations.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered::Reason::store_access_lapsed PSN refused the
  ///     app token even after it was renewed.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto conversations(users::PsUser& user,
                                   const platform::StopToken& stop)
      -> Expected<std::vector<Conversation>, users::Unanswered>;
  /// @brief Reads a conversation's newest messages, or the ones before a
  /// message.
  ///
  /// @param user The signed-in user.
  /// @param group_id The conversation's Conversation::group_id.
  /// @param thread_id The conversation's Conversation::thread_id.
  /// @param before A ChatMessage::id to read before; empty for the newest.
  /// @param stop Cancels the request.
  /// @return The messages, oldest first.
  /// @retval users::Unanswered::Reason::rejected The group or thread is
  ///     empty.
  /// @retval users::Unanswered The failures conversations() lists.
  [[nodiscard]] auto thread(users::PsUser& user, std::string_view group_id,
                            std::string_view thread_id, std::string_view before,
                            const platform::StopToken& stop)
      -> Expected<ChatThread, users::Unanswered>;
  /// @brief Sends text in a conversation.
  ///
  /// @param user The signed-in user.
  /// @param group_id The conversation's Conversation::group_id.
  /// @param thread_id The conversation's Conversation::thread_id.
  /// @param text The text; surrounding whitespace is dropped.
  /// @param stop Cancels the request.
  /// @return The message as sent, from the user, at the device's time.
  /// @retval users::Unanswered::Reason::rejected The text is empty, not
  ///     UTF-8 or longer than longest_text UTF-16 units, or the group or thread
  ///     is empty, so nothing was sent; or PSN rejected the message.
  /// @retval users::Unanswered The failures conversations() lists.
  [[nodiscard]] auto send(users::PsUser& user, std::string_view group_id,
                          std::string_view thread_id, std::string_view text,
                          const platform::StopToken& stop)
      -> Expected<ChatMessage, users::Unanswered>;
  /// @brief Marks a conversation read up to a message.
  ///
  /// @param user The signed-in user.
  /// @param group_id The conversation's Conversation::group_id.
  /// @param thread_id The conversation's Conversation::thread_id.
  /// @param message_id The newest ChatMessage::id shown.
  /// @param stop Cancels the request.
  /// @return Nothing once PSN accepted it.
  /// @retval users::Unanswered::Reason::rejected An id is empty.
  /// @retval users::Unanswered The failures conversations() lists.
  [[nodiscard]] auto mark_read(users::PsUser& user, std::string_view group_id,
                               std::string_view thread_id,
                               std::string_view message_id,
                               const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Builds the service.
  ///
  /// @param http Sends every request to PSN. Must outlive the service.
  /// @param clock The monotonic clock requests are timed by. Must outlive
  ///     the service.
  /// @param wall_clock Device time, which a sent message is stamped with.
  ///     Must outlive the service.
  /// @param diagnostics Receives log lines; may be null to log nothing. Must
  ///     outlive the service when given.
  Messages(platform::HttpProvider& http, const platform::ClockProvider& clock,
           const platform::ClockProvider& wall_clock,
           platform::DiagnosticsProvider* diagnostics = nullptr) noexcept;

  /// @brief Sends every request.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Device time.
  const platform::ClockProvider& wall_clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::messages
