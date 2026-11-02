#include "src/core/catalog/adapters/ps/cloud_game_resolver.hpp"

#include "src/core/users/ports/catalog_calls.hpp"

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

CloudGameResolver::CloudGameResolver(
    users::PsUser& user, net::http::Client& http,
    std::size_t maximum_response_bytes) noexcept
    : user_(user),
      http_(http),
      maximum_response_bytes_(maximum_response_bytes) {}

auto CloudGameResolver::for_product(const ProductId& product,
                                    std::span<const users::BenefitPlanId> plans)
    -> cloud_stream::CloudGameListing {
  return resolved(psn::rest::CloudGameQuestion::Asking::product_id,
                  product.text(), plans);
}

auto CloudGameResolver::resolved(psn::rest::CloudGameQuestion::Asking asking,
                                 std::string_view id,
                                 std::span<const users::BenefitPlanId> plans)
    -> cloud_stream::CloudGameListing {
  using Listing = cloud_stream::CloudGameListing;
  if (plans.empty()) return Listing::not_asked(Listing::Unasked::no_plan_ids);
  const auto context = user_.account_context();
  const auto& country = context.legal_country();
  const auto language = context.language();
  if (!country || !language)
    return Listing::not_asked(Listing::Unasked::no_account_locale);
  std::vector<std::string> plan_ids;
  plan_ids.reserve(plans.size());
  for (const auto& plan : plans) plan_ids.push_back(plan.text());
  const auto reply = users::CatalogCalls{user_}.cloud_game_product(
      http_,
      {.asking = asking,
       .id = id,
       .country = *country,
       .language = *language,
       .benefit_plan_ids = plan_ids},
      maximum_response_bytes_);
  if (!reply) return Listing::failed(Unanswered{reply.error()});
  if (!reply.value().first)
    return Listing::not_listed(reply.value().http_status);
  return Listing::listed(product_of(*reply.value().first),
                         reply.value().http_status);
}

auto CloudGameResolver::product_of(const psn::rest::CloudGameProduct& listed)
    -> cloud_stream::CloudGameListing::Product {
  cloud_stream::CloudGameListing::Product product{
      .id = ProductId::parse(listed.product_id)};
  if (!listed.titles) return product;
  product.titles.emplace();
  for (const auto& title : *listed.titles)
    product.titles->push_back(
        {.title_id = TitleId::parse(title.title_id),
         .entitlement = OwnershipId::parse(title.entitlement_id),
         .native_platform = title.native_platform,
         .downloadable = title.is_downloadable,
         .game_intent = title.game_intent});
  return product;
}

}  // namespace libchiaking::catalog::ps
