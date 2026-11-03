#include "src/core/psn/rest/subscriptions/subscriptions_client.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

struct SubscriptionsClient::CloudGameReply {
  struct Owner {
    using keys = json::ps::CamelCaseKeys;
    json::ps::DecimalText owner_account_id;
    json::ps::Tolerant<std::string> tier_label;
    std::optional<std::vector<std::string>> benefit_plan_ids;
    json::ps::Tolerant<bool> has_est_benefit;
    json::ps::Tolerant<bool> has_sgod_benefit;

    auto owner() && -> CloudGameOwner {
      return CloudGameOwner{
          .owner_account_id = owner_account_id.text(),
          .tier_label = std::move(tier_label).value_or(""),
          .benefit_plan_ids =
              std::move(benefit_plan_ids).value_or(std::vector<std::string>{}),
          .has_est_benefit = has_est_benefit.value_or(false),
          .has_sgod_benefit = has_sgod_benefit.value_or(false)};
    }
  };
  using keys = json::ps::CamelCaseKeys;
  std::optional<bool> authorization;
  std::optional<glz::raw_json> justification;
  std::optional<std::vector<Owner>> cloudgame_owner_infos;

  auto justification_text() const -> std::optional<std::string> {
    if (!justification) return std::nullopt;
    std::string words;
    if (justification->str.starts_with('"') &&
        !glz::read<json::ps::reply_options>(words, justification->str))
      return words;
    return justification->str;
  }

  auto feature() && -> CloudGameFeature {
    CloudGameFeature read{.authorization = authorization,
                          .justification = justification_text()};
    if (cloudgame_owner_infos)
      for (auto& owner : *cloudgame_owner_infos)
        read.owners.push_back(std::move(owner).owner());
    return read;
  }
};

struct SubscriptionsClient::ServicesReply {
  struct State {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<bool> active;
    json::ps::Tolerant<std::string> status;
  };
  struct Tier {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> tier_label;
  };
  struct Service {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> type;
    json::ps::Tolerant<State> state;
    json::ps::Tolerant<Tier> current_tier;

    auto plus() const -> bool { return type.value_or("") == plus_service_type; }

    auto service_state() && -> PlusServiceState {
      return PlusServiceState{
          .active = state && state->active.value_or(false),
          .status =
              state ? std::move(state->status).value_or("") : std::string{},
          .tier_label = current_tier
                            ? std::move(current_tier->tier_label).value_or("")
                            : std::string{}};
    }
  };
  using Services = std::vector<json::ps::Tolerant<Service>>;
};

SubscriptionsClient::SubscriptionsClient(net::http::Client& client,
                                         auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto SubscriptionsClient::cloud_game()
    -> Expected<CloudGameFeature, CloudGameError> {
  auto reply = fetched<CloudGameReply>(cloud_game_endpoint,
                                       net::http::Query{cloud_game_url});
  if (!reply) return unexpected(std::move(reply.error()));
  return std::move(reply.value()).feature();
}

auto SubscriptionsClient::first_plus_service()
    -> Expected<std::optional<PlusServiceState>, PlusServiceError> {
  net::http::Query url{services_url};
  url.add_verbatim("days", "1").add_verbatim("useTiers", "true");
  auto reply = fetched<json::ps::Tolerant<ServicesReply::Services>>(
      services_endpoint, std::move(url));
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply.value()) return std::optional<PlusServiceState>{};
  auto& services = *reply.value();
  const auto first = std::ranges::find_if(
      services, [](const auto& service) { return service && service->plus(); });
  if (first == services.end()) return std::optional<PlusServiceState>{};
  return std::optional{std::move(**first).service_state()};
}

template <class Reply>
auto SubscriptionsClient::fetched(const net::http::Endpoint& endpoint,
                                  net::http::Query url)
    -> Expected<Reply, FetchError> {
  net::http::Request request{endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  return FetchError::checked_reply<Reply>(client_.send(std::move(request)),
                                          token_.view());
}

}  // namespace libchiaking::psn::rest
