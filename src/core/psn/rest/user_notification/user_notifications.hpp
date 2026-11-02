/// @file
/// @brief Declares UserNotifications, the PSN notification list that holds
/// game invitations, read with the app token.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief How many notifications the list asks for.
inline constexpr std::size_t notifications_page_size = 100;

/// @brief One entry of the account's notification list. Plain data; text
/// fields are empty when PS omitted them.
struct UserNotification {
  /// @brief The `notificationId`.
  std::string notification_id;
  /// @brief The `useCaseId`, such as `NUC3` for a game invitation.
  std::string use_case_id;
  /// @brief The `state`: `New`, `Seen`, `Read` or `Deleted`.
  std::string state;
  /// @brief The `expirationDateTime` as PS wrote it.
  std::string expires;
  /// @brief The `updatedDateTime` as PS wrote it.
  std::string updated;
  /// @brief The sender's `fromUser.accountId` in decimal.
  std::string from_account_id;
  /// @brief The sender's `fromUser.onlineId`, which PS often omits.
  std::string from_online_id;
  /// @brief The `associatedTitle.npTitleId`, or the `associatedTitle` in
  /// `viewData.parameters` when that is blank.
  std::string title_id;
  /// @brief Every title in `viewData.parameters.associatedTitles`: for a
  /// game invitation, all the PS4 and PS5 titles of the game's concept.
  std::vector<std::string> title_ids;
  /// @brief The player session in `viewData.parameters.sessionId`.
  std::string session_id;
  /// @brief The invitation in `viewData.parameters.invitationId`.
  std::string invitation_id;
};

/// @brief Reads the account's notification list with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class UserNotifications {
 public:
  /// @brief The failure of list().
  using ListError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of set_state().
  using SetStateError = Error<bad_request::InvalidMessage>;
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  UserNotifications(net::http::Client& client,
                    auth::BearerToken token) noexcept;

  /// @brief Reads the newest notifications.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userNotificationManager/v1/users/me/notifications?offset=0&limit=100&templateVersion=23.06`,
  /// a single page, with the `Accept-Language` header PS
  /// requires; unreadable entries are skipped.
  ///
  /// @param language The locale to send, such as `en-GB`.
  /// @return The notifications, in reply order.
  /// @retval bad_reply::InvalidSchema The reply has no `notifications`.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto list(std::string_view language)
      -> Expected<std::vector<UserNotification>, ListError>;
  /// @brief Moves notifications to a state, which deletes them or marks them
  /// read or seen.
  ///
  /// PUT the notifications address with
  /// `{"notifications":[{"notificationId":…,"state":…}]}`, one entry per id.
  ///
  /// @param notification_ids The notifications' `notificationId`s; nothing
  ///     is sent when empty.
  /// @param state `Deleted`, `Read` or `Seen`.
  /// @return Nothing once PS accepted the change.
  /// @retval bad_request::InvalidMessage The body could not be written.
  /// @retval Error A transport failure or an error status.
  [[nodiscard]] auto set_state(std::span<const std::string> notification_ids,
                               std::string_view state)
      -> Expected<void, SetStateError>;

 private:
  /// @brief The account's notifications.
  static constexpr std::string_view notifications_url =
      "https://m.np.playstation.com/api/userNotificationManager/v1/users/me/"
      "notifications";
  /// @brief The template version game invitations are asked with.
  static constexpr std::string_view template_version = "23.06";

  /// @brief The request, logged with its query.
  static constexpr net::http::Endpoint list_endpoint{
      .label = "psn.user_notification.list",
      .log_query = true,
  };

  /// @brief The state change request.
  static constexpr net::http::Endpoint set_state_endpoint{
      .label = "psn.user_notification.set_state",
      .method = net::http::Method::put,
  };

  /// @brief The reply's `notifications`.
  struct Reply;
  /// @brief The state change body.
  struct StateBody;

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
