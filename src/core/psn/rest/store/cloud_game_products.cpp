#include "src/core/psn/rest/store/cloud_game_products.hpp"

#include "src/core/net/transport/http/percent_encoding.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/glaze.hpp>

#include <ranges>
#include <utility>

namespace libchiaking::psn::rest {

struct CloudGameProducts::ListedTitle {
  using keys = json::ps::CamelCaseKeys;
  json::ps::Tolerant<std::string> title_id;
  json::ps::Tolerant<std::string> entitlement_id;
  json::ps::Tolerant<std::string> native_platform;
  json::ps::Tolerant<bool> is_downloadable;
  json::ps::Tolerant<std::string> game_intent;
};

struct CloudGameProducts::ListedProduct {
  using keys = json::ps::CamelCaseKeys;
  json::ps::Tolerant<std::string> product_id;
  std::optional<std::vector<ListedTitle>> titles;
};

struct CloudGameProducts::Reply {
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::vector<glz::raw_json>> product_info;
};

CloudGameProducts::CloudGameProducts(net::http::Client& client,
                                     auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto CloudGameProducts::first_listed(const CloudGameQuestion& question)
    -> Expected<CloudGameReply, FirstListedError> {
  net::http::Request request{endpoint, query_of(question)};
  request.authorize(token_).header("Accept", "application/json");
  auto response = FirstListedError::checked(client_.send(std::move(request)),
                                            token_.view());
  if (!response) return unexpected(std::move(response.error()));
  auto listed = FirstListedError::reply_of<Reply>(response.value());
  if (!listed) return unexpected(std::move(listed.error()));
  CloudGameReply reply{.http_status = response.value().status()};
  if (!listed.value().product_info || listed.value().product_info->empty())
    return reply;
  auto first = json::ps::read_reply<ListedProduct>(
      listed.value().product_info->front().str);
  if (!first) return unexpected(FirstListedError{bad_reply::InvalidSchema{}});
  reply.first = product_of(std::move(first.value()));
  return reply;
}

auto CloudGameProducts::query_of(const CloudGameQuestion& question)
    -> net::http::Query {
  std::string plans;
  for (const auto& plan : question.benefit_plan_ids) {
    if (!plans.empty()) plans += ',';
    plans += net::http::PercentEncoding::component(plan);
  }
  net::http::Query query{url};
  query
      .add(question.asking == CloudGameQuestion::Asking::product_id
               ? "productId"
               : "entitlementId",
           question.id)
      .add("country", question.country)
      .add("language", question.language)
      .add("age", std::to_string(age))
      .add("limit", std::to_string(limit))
      .add_verbatim("benefitPlanIds", plans)
      .add_verbatim("homeshare", "false")
      .add_verbatim("storeFront", "PS5");
  return query;
}

auto CloudGameProducts::product_of(ListedProduct listed) -> CloudGameProduct {
  CloudGameProduct product{.product_id =
                               std::move(listed.product_id).value_or("")};
  if (listed.titles)
    product.titles = std::move(*listed.titles) | std::views::as_rvalue |
                     std::views::transform(&CloudGameProducts::title_of) |
                     std::ranges::to<std::vector>();
  return product;
}

auto CloudGameProducts::title_of(ListedTitle listed) -> CloudGameTitle {
  return CloudGameTitle{
      .title_id = std::move(listed.title_id).value_or(""),
      .entitlement_id = std::move(listed.entitlement_id).value_or(""),
      .native_platform = std::move(listed.native_platform).value_or(""),
      .is_downloadable = listed.is_downloadable
                             ? std::optional{*listed.is_downloadable}
                             : std::nullopt,
      .game_intent = listed.game_intent
                         ? std::optional{std::move(*listed.game_intent)}
                         : std::nullopt};
}

}  // namespace libchiaking::psn::rest
