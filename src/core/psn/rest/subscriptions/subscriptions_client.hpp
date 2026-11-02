/// @file
/// @brief Declares SubscriptionsClient, the PSN subscriptions service that
/// PS5 cloud streaming access is judged from.
///
/// The cloud game feature says whether the account may stream and under which
/// benefit plans; the PlayStation Plus service, read only when streaming is
/// refused, tells an upgrade apart from a new subscription.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief One holder of the cloud game benefit, from `cloudgameOwnerInfos`.
/// Plain data.
struct CloudGameOwner {
  /// @brief The owner's account id in decimal, whether PS sent a number or
  /// a string; empty when absent.
  std::string owner_account_id;
  /// @brief The owner's `tierLabel`, such as `TIER_30`; empty when absent.
  std::string tier_label;
  /// @brief The owner's `benefitPlanIds`, which the cloud game resolver is
  /// asked with; empty when absent.
  std::vector<std::string> benefit_plan_ids;
  /// @brief PS's `hasEstBenefit` flag; false when absent. Kept as evidence.
  bool has_est_benefit{};
  /// @brief PS's `hasSgodBenefit` flag; false when absent. Kept as
  /// evidence.
  bool has_sgod_benefit{};
};

/// @brief The account's cloud game feature. Plain data.
struct CloudGameFeature {
  /// @brief The `authorization` flag: whether the account may stream; nothing
  /// when absent.
  std::optional<bool> authorization;
  /// @brief PS's `justification`: the text of a JSON string, or the raw
  /// JSON of any other value; nothing when absent.
  std::optional<std::string> justification;
  /// @brief Every owner, in reply order.
  std::vector<CloudGameOwner> owners;
};

/// @brief The account's first PlayStation Plus service. Plain data.
struct PlusServiceState {
  /// @brief The service's `state.active`; false when absent.
  bool active{};
  /// @brief The service's `state.status`, such as `SUBSCRIBED`; empty when
  /// absent.
  std::string status;
  /// @brief The `currentTier.tierLabel`, such as `TIER_10`; empty when
  /// absent.
  std::string tier_label;
};

/// @brief Reads the subscriptions service with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class SubscriptionsClient {
 public:
  /// @brief The failure of a read with fetched().
  using FetchError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of first_plus_service().
  using PlusServiceError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of cloud_game().
  using CloudGameError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The cloud game feature request's diagnostic label.
  static constexpr std::string_view cloud_game_label =
      "psn.subscriptions.cloud_game";
  /// @brief The services request's diagnostic label.
  static constexpr std::string_view services_label =
      "psn.subscriptions.services";

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  SubscriptionsClient(net::http::Client& client,
                      auth::BearerToken token) noexcept;

  /// @brief Reads the cloud game feature.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/subscriptions/v2/users/me/premium/features/cloudgame`
  /// with no query; `authorization`, `justification` and
  /// `cloudgameOwnerInfos` are read.
  ///
  /// @return The feature.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto cloud_game() -> Expected<CloudGameFeature, CloudGameError>;
  /// @brief Reads the first PlayStation Plus service on the account.
  ///
  /// GET
  /// `.../subscriptions/v2/users/me/services/pssubscriptions?days=1&useTiers=true`;
  /// the reply is an array of services, of which the first of `type` `PSPLUS`
  /// is read. What `days=1` means to the service is not known.
  ///
  /// @return The service; nothing when the reply is not an array or holds no
  ///     PlayStation Plus service.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto first_plus_service()
      -> Expected<std::optional<PlusServiceState>, PlusServiceError>;

 private:
  /// @brief The cloud game feature reply's fields.
  struct CloudGameReply;
  /// @brief The services reply's fields.
  struct ServicesReply;

  /// @brief The cloud game feature endpoint.
  static constexpr std::string_view cloud_game_url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/premium/"
      "features/cloudgame";
  /// @brief The subscription services endpoint.
  static constexpr std::string_view services_url =
      "https://m.np.playstation.com/api/subscriptions/v2/users/me/services/"
      "pssubscriptions";
  /// @brief The `type` of a PlayStation Plus service.
  static constexpr std::string_view plus_service_type = "PSPLUS";

  /// @brief The cloud game feature request.
  static constexpr net::http::Endpoint cloud_game_endpoint{
      .label = cloud_game_label,
  };
  /// @brief The services request, logged with its query.
  static constexpr net::http::Endpoint services_endpoint{
      .label = services_label,
      .log_query = true,
  };

  /// @brief Sends a GET with the token and decodes a 2xx reply.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param endpoint The endpoint.
  /// @param url The URL with its query.
  /// @return The decoded reply.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  template <class Reply>
  [[nodiscard]] auto fetched(const net::http::Endpoint& endpoint,
                             net::http::Query url)
      -> Expected<Reply, FetchError>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
