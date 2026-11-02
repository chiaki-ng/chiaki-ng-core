/// @file
/// @brief Declares MessageCalls, the messages context's port onto a user's
/// authorised calls to PS.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/gaming_lounge/lounge_groups.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::users {

class AuthorisedRequest;

/// @brief What the messages context may ask PS on a user's behalf: chat
/// groups, their messages, sending and marking read.
///
/// A user that is off PSN is answered at once and nothing is sent. A failure
/// that means PSN no longer accepts the NPSSO marks the user's sign-in as
/// refused, so the user reports SignInState::expired.
///
/// The object is a short-lived view: it only refers to the user's core, holds
/// no state of its own, and is made afresh wherever a call is needed. It must
/// not outlive the PsUser it was made for. Every call blocks the calling
/// thread for its requests, each bounded by the HTTP client's timeouts and its
/// stop token; calls may be made from several threads at once, the user's core
/// providing the locking.
class MessageCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit MessageCalls(PsUser& user) noexcept;

  /// @brief Reads one page of the user's conversations, signed with the app
  /// token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param language The locale to send, such as `en-GB`.
  /// @param offset Index of the first conversation.
  /// @param favorite True for the favourite conversations, false for the
  ///     rest.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto chat_groups(net::http::Client& http,
                                 std::string_view language, std::size_t offset,
                                 bool favorite)
      -> Expected<psn::rest::ChatGroupsPage, Unanswered>;

  /// @brief Reads a conversation's messages, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param language The locale to send, such as `en-GB`.
  /// @param group_id The conversation's group.
  /// @param thread_id The conversation's thread.
  /// @param before The message to read before; empty for the newest.
  /// @return The messages, in reply order.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto chat_messages(net::http::Client& http,
                                   std::string_view language,
                                   std::string_view group_id,
                                   std::string_view thread_id,
                                   std::string_view before)
      -> Expected<std::vector<psn::rest::ChatMessageRow>, Unanswered>;

  /// @brief Sends a text message, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param language The locale to send, such as `en-GB`.
  /// @param group_id The conversation's group.
  /// @param thread_id The conversation's thread.
  /// @param text The text.
  /// @return The new message's id; empty when PS named none.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto send_chat_message(net::http::Client& http,
                                       std::string_view language,
                                       std::string_view group_id,
                                       std::string_view thread_id,
                                       std::string_view text)
      -> Expected<std::string, Unanswered>;

  /// @brief Marks a conversation read up to a message, signed with the app
  /// token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param language The locale to send, such as `en-GB`.
  /// @param group_id The conversation's group.
  /// @param thread_id The conversation's thread.
  /// @param message_uid The newest message read.
  /// @return Nothing once PS accepted it.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto mark_chat_read(net::http::Client& http,
                                    std::string_view language,
                                    std::string_view group_id,
                                    std::string_view thread_id,
                                    std::string_view message_uid)
      -> Expected<void, Unanswered>;

 private:
  /// @brief Opens the shared request helpers on this user.
  ///
  /// @return A view of the same user.
  auto request() const noexcept -> AuthorisedRequest;

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
