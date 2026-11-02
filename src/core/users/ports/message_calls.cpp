#include "src/core/users/ports/message_calls.hpp"

#include "src/core/users/ports/authorised_request.hpp"

#include <utility>

namespace libchiaking::users {

MessageCalls::MessageCalls(PsUser& user) noexcept
    : user_(PsUser::Core::of(user)) {}

auto MessageCalls::request() const noexcept -> AuthorisedRequest {
  return AuthorisedRequest{user_};
}

auto MessageCalls::chat_groups(net::http::Client& http,
                               std::string_view language, std::size_t offset,
                               bool favorite)
    -> Expected<psn::rest::ChatGroupsPage, Unanswered> {
  return request().asked_of_console_directory<psn::rest::ChatGroupsPage>(
      http,
      [&](psn::auth::BearerToken token) {
        return psn::rest::LoungeGroups{http, std::move(token),
                                       std::string{language}}
            .groups(offset, favorite);
      },
      AuthorisedRequest::TokenRefusal::unauthorised_only);
}

auto MessageCalls::chat_messages(net::http::Client& http,
                                 std::string_view language,
                                 std::string_view group_id,
                                 std::string_view thread_id,
                                 std::string_view before)
    -> Expected<std::vector<psn::rest::ChatMessageRow>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::ChatMessageRow>>(
          http,
          [&](psn::auth::BearerToken token) {
            return psn::rest::LoungeGroups{http, std::move(token),
                                           std::string{language}}
                .messages(group_id, thread_id, before);
          },
          AuthorisedRequest::TokenRefusal::unauthorised_only);
}

auto MessageCalls::send_chat_message(net::http::Client& http,
                                     std::string_view language,
                                     std::string_view group_id,
                                     std::string_view thread_id,
                                     std::string_view text)
    -> Expected<std::string, Unanswered> {
  return request().asked_of_console_directory<std::string>(
      http,
      [&](psn::auth::BearerToken token) {
        return psn::rest::LoungeGroups{http, std::move(token),
                                       std::string{language}}
            .send(group_id, thread_id, text);
      },
      AuthorisedRequest::TokenRefusal::unauthorised_only);
}

auto MessageCalls::mark_chat_read(net::http::Client& http,
                                  std::string_view language,
                                  std::string_view group_id,
                                  std::string_view thread_id,
                                  std::string_view message_uid)
    -> Expected<void, Unanswered> {
  auto marked = request().asked_of_console_directory<bool>(
      http,
      [&](psn::auth::BearerToken token)
          -> Expected<bool, psn::rest::LoungeGroups::MarkReadError> {
        auto done = psn::rest::LoungeGroups{http, std::move(token),
                                            std::string{language}}
                        .mark_read(group_id, thread_id, message_uid);
        if (!done) return unexpected(std::move(done.error()));
        return true;
      },
      AuthorisedRequest::TokenRefusal::unauthorised_only);
  if (!marked) return unexpected(marked.error());
  return {};
}

}  // namespace libchiaking::users
