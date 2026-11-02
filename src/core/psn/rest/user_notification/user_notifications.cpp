#include "src/core/psn/rest/user_notification/user_notifications.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <string>
#include <utility>

namespace libchiaking::psn::rest {

namespace ps = json::ps;

struct UserNotifications::Reply {
  struct User {
    using keys = ps::CamelCaseKeys;
    ps::DecimalText account_id;
    ps::Tolerant<std::string> online_id;
  };
  struct Title {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> np_title_id;
  };
  struct Parameters {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> session_id;
    ps::Tolerant<std::string> invitation_id;
    ps::Tolerant<std::string> associated_title;
    ps::Tolerant<std::vector<ps::Tolerant<std::string>>> associated_titles;
  };
  struct View {
    ps::Tolerant<Parameters> parameters;
  };
  struct Entry {
    using keys = ps::CamelCaseKeys;
    ps::Tolerant<std::string> notification_id;
    ps::Tolerant<std::string> use_case_id;
    ps::Tolerant<std::string> state;
    ps::Tolerant<std::string> expiration_date_time;
    ps::Tolerant<std::string> updated_date_time;
    ps::Tolerant<User> from_user;
    ps::Tolerant<Title> associated_title;
    ps::Tolerant<View> view_data;
  };
  std::optional<std::vector<ps::Tolerant<Entry>>> notifications;
};

UserNotifications::UserNotifications(net::http::Client& client,
                                     auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

struct UserNotifications::StateBody {
  struct Change {
    using keys = ps::CamelCaseKeys;
    std::string_view notification_id;
    std::string_view state;
  };
  std::vector<Change> notifications;
};

auto UserNotifications::set_state(std::span<const std::string> notification_ids,
                                  std::string_view state)
    -> Expected<void, SetStateError> {
  if (notification_ids.empty()) return {};
  StateBody changes;
  changes.notifications.reserve(notification_ids.size());
  for (const auto& id : notification_ids)
    changes.notifications.push_back({id, state});
  auto body = ps::write(changes);
  if (!body) return unexpected(SetStateError{bad_request::InvalidMessage{}});
  net::http::Request request{set_state_endpoint,
                             net::http::Query{notifications_url}};
  request.authorize(token_)
      .header("Accept", "application/json")
      .body("application/json", SecretBytes{body->bytes()});
  auto sent =
      SetStateError::checked(client_.send(std::move(request)), token_.view());
  if (!sent) return unexpected(std::move(sent.error()));
  return {};
}

auto UserNotifications::list(std::string_view language)
    -> Expected<std::vector<UserNotification>, ListError> {
  net::http::Query url{notifications_url};
  url.add_verbatim("offset", "0")
      .add_verbatim("limit", std::to_string(notifications_page_size))
      .add_verbatim("templateVersion", template_version);
  net::http::Request request{list_endpoint, std::move(url)};
  request.authorize(token_)
      .header("Accept", "application/json")
      .header("Accept-Language", language);
  auto reply = ListError::checked_reply<Reply>(client_.send(std::move(request)),
                                               token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply->notifications)
    return unexpected(ListError{bad_reply::InvalidSchema{}});
  std::vector<UserNotification> found;
  for (auto& slot : *reply->notifications) {
    if (!slot) continue;
    auto& entry = *slot;
    UserNotification notification{
        .notification_id = std::move(entry.notification_id).value_or(""),
        .use_case_id = std::move(entry.use_case_id).value_or(""),
        .state = std::move(entry.state).value_or(""),
        .expires = std::move(entry.expiration_date_time).value_or(""),
        .updated = std::move(entry.updated_date_time).value_or("")};
    if (entry.from_user) {
      notification.from_account_id = entry.from_user->account_id.trimmed();
      notification.from_online_id =
          std::move(entry.from_user->online_id).value_or("");
    }
    if (entry.associated_title)
      notification.title_id =
          std::move(entry.associated_title->np_title_id).value_or("");
    if (entry.view_data && entry.view_data->parameters) {
      auto& parameters = *entry.view_data->parameters;
      notification.session_id = std::move(parameters.session_id).value_or("");
      notification.invitation_id =
          std::move(parameters.invitation_id).value_or("");
      if (text::trim(notification.title_id).empty())
        notification.title_id =
            std::move(parameters.associated_title).value_or("");
      if (parameters.associated_titles)
        for (auto& title : *parameters.associated_titles)
          if (title && !text::trim(*title).empty())
            notification.title_ids.push_back(std::move(*title));
    }
    found.push_back(std::move(notification));
  }
  return found;
}

}  // namespace libchiaking::psn::rest
