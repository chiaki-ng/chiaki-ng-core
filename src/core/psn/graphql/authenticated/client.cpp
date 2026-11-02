#include "src/core/psn/graphql/authenticated/client.hpp"

#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::psn::graphql {

auto AuthenticatedClient::streams_on_ps5(
    const std::vector<std::string>& platforms) -> bool {
  return std::ranges::any_of(platforms, [](const std::string& platform) {
    return platform == "PS5" || platform == "DWC";
  });
}

auto AuthenticatedClient::streaming_of(NoticesByPlatform& notices)
    -> TitleStreaming {
  TitleStreaming result;
  if (!notices) return result;
  for (auto& [platform, list] : *notices) {
    if (!list) continue;
    for (auto& notice : *list) {
      if (!notice) continue;
      result.notices = true;
      if (notice->type.value_or(std::string{}) != streaming_notice) continue;
      auto targets = std::move(notice->target_platforms).values();
      const bool ps5 = streams_on_ps5(targets);
      if (result.supported || (result.listed && !ps5)) continue;
      result.listed = true;
      result.supported = ps5;
      result.platforms = std::move(targets);
      result.value = std::move(notice->value).take();
    }
  }
  return result;
}

void AuthenticatedClient::take_param(TitleOffer& offer, Param& param) {
  const auto name = param.name.value_or(std::string{});
  auto values = std::move(param.values).values();
  if (name == "skuId" && offer.sku_id.empty() && !values.empty())
    offer.sku_id = std::move(values.front());
  else if (name == "entitlementIds")
    std::ranges::move(values, std::back_inserter(offer.entitlement_ids));
  else if (name == "rewardId" && offer.reward_id.empty() && !values.empty())
    offer.reward_id = std::move(values.front());
  else if (name == "isEntitledViaSubscription" && !values.empty() &&
           !offer.entitled_via_subscription) {
    if (values.front() == "true")
      offer.entitled_via_subscription = true;
    else if (values.front() == "false")
      offer.entitled_via_subscription = false;
  }
}

auto AuthenticatedClient::offer_of(Cta& cta) -> TitleOffer {
  TitleOffer offer;
  offer.cta_type = std::move(cta.type).take();
  offer.cloud_streaming_message = std::move(cta.cloud_streaming_message).take();
  auto action = std::move(cta.action).value_or(Action{});
  offer.action_type = std::move(action.type).take();
  if (action.param)
    for (auto& param : *action.param)
      if (param) take_param(offer, *param);
  auto meta = std::move(cta.meta).value_or(Meta{});
  offer.pre_order = meta.pre_order.value_or(false);
  if (meta.ineligibility_reasons)
    for (auto& reason : *meta.ineligibility_reasons)
      if (reason && !reason->type.empty())
        offer.ineligibility_reasons.push_back(std::move(reason->type).take());
  auto price = std::move(cta.price).value_or(Price{});
  offer.base_price = std::move(price.base_price).take();
  offer.discounted_price = std::move(price.discounted_price).take();
  offer.currency_code = std::move(price.currency_code).take();
  offer.base_price_value = price.base_price_value.value_or(0);
  offer.discounted_value = price.discounted_value.value_or(0);
  offer.is_free = price.is_free.value_or(false);
  offer.tied_to_subscription = price.is_tied_to_subscription.value_or(false);
  offer.service_branding = std::move(price.service_branding).values();
  return offer;
}

auto AuthenticatedClient::offers_of(TolerantList<Cta>& ctas)
    -> std::vector<TitleOffer> {
  std::vector<TitleOffer> offers;
  if (!ctas) return offers;
  for (auto& cta : *ctas)
    if (cta) offers.push_back(offer_of(*cta));
  return offers;
}

auto AuthenticatedClient::product_of(Tolerant<Product>& value)
    -> std::optional<TitleProduct> {
  if (!value || value->id.empty()) return std::nullopt;
  auto& listed = *value;
  TitleProduct product;
  product.id = std::move(listed.id).take();
  product.name = std::move(listed.name).take();
  product.np_title_id = std::move(listed.np_title_id).take();
  product.platforms = std::move(listed.platforms).values();
  product.release_date = std::move(listed.release_date).text();
  product.classification =
      std::move(listed.store_display_classification).take();
  if (listed.edition) product.edition = std::move(listed.edition->name).take();
  product.streaming = streaming_of(listed.compatibility_notices_by_platform);
  if (listed.descriptions)
    for (auto& description : *listed.descriptions)
      if (description &&
          description->type.value_or(std::string{}) ==
              "STREAMING_COMPATIBILITY_NOTICE" &&
          product.streaming_notice.empty())
        product.streaming_notice = std::move(description->value).take();
  product.offers = offers_of(listed.mobilectas);
  product.media = listed.media.title_media();
  return product;
}

auto AuthenticatedClient::identities_of(Identities& values)
    -> std::optional<std::vector<std::string>> {
  if (!values) return std::nullopt;
  std::vector<std::string> result;
  for (auto& value : *values) {
    if (!value) continue;
    if (auto* text = std::get_if<std::string>(&*value)) {
      result.push_back(std::move(*text));
      continue;
    }
    auto& titled = std::get<Titled>(*value);
    for (auto* field : {&titled.product_id, &titled.id, &titled.entitlement_id,
                        &titled.title_id, &titled.sku_id})
      if (!field->empty()) result.emplace_back(field->view());
  }
  return result;
}

auto AuthenticatedClient::ownership_of(Concept& grouped)
    -> std::optional<TitleDetails::Ownership> {
  auto owned = identities_of(grouped.owned_titles);
  if (!grouped.selectable_products) return std::nullopt;
  auto purchased = identities_of(grouped.selectable_products->purchased_titles);
  if (!owned || !purchased ||
      !grouped.selectable_products->purchasable_products)
    return std::nullopt;
  return TitleDetails::Ownership{std::move(*owned), std::move(*purchased)};
}

auto AuthenticatedClient::concept_details(Concept& grouped)
    -> std::optional<TitleDetails> {
  TitleDetails::Summary summary{
      .concept_id = std::string{grouped.id.view()},
      .name = std::string{grouped.name.view()},
      .publisher = std::string{grouped.publisher_name.view()},
      .release_date = std::move(grouped.release_date).text(),
      .genres = std::move(grouped.combined_localized_genres).names(),
      .rating = std::move(grouped.content_rating).value_or(ContentRating{}),
      .streaming = streaming_of(grouped.compatibility_notices_by_platform),
      .media = grouped.media.title_media()};
  auto details = TitleDetails::of(std::move(summary));
  if (!details) return std::nullopt;
  if (auto parsed = product_of(grouped.default_product))
    details->add(std::move(*parsed));
  return details;
}

auto AuthenticatedClient::priced(TitleDetails details, Concept& grouped)
    -> std::optional<TitleDetails> {
  if (grouped.id.view() != details.concept_id()) return std::nullopt;
  auto ownership = ownership_of(grouped);
  if (!ownership) return std::nullopt;
  if (auto parsed = product_of(grouped.default_product))
    details.add(std::move(*parsed));
  for (auto& product : *grouped.selectable_products->purchasable_products)
    if (auto parsed = product_of(product)) details.add(std::move(*parsed));
  details.settle(std::move(*ownership));
  return details;
}

auto AuthenticatedClient::by_product(ProductRoot& root,
                                     std::string_view product_id)
    -> std::optional<TitleDetails> {
  if (TitleDetails::sku_base(root.id.view()) !=
          TitleDetails::sku_base(product_id) ||
      !root.grouped)
    return std::nullopt;
  auto& grouped = *root.grouped;
  auto details = concept_details(grouped);
  if (!details) return std::nullopt;
  auto ownership = ownership_of(grouped);
  if (!ownership) return std::nullopt;
  auto& selectable = *grouped.selectable_products;
  for (auto& product : *selectable.purchasable_products)
    if (auto parsed = product_of(product)) details->add(std::move(*parsed));
  for (auto& title : *selectable.purchased_titles)
    if (title)
      if (auto* titled = std::get_if<Titled>(&*title))
        if (auto parsed = product_of(titled->product))
          details->add(std::move(*parsed));
  if (!details->lists(product_id)) return std::nullopt;
  details->offer(product_id, offers_of(root.mobilectas));
  details->settle(std::move(*ownership));
  return details;
}

auto AuthenticatedClient::single_field(std::string_view key,
                                       std::string_view value) -> Variables {
  return Variables::of(std::pair{key, text::c_string(value)});
}

auto AuthenticatedClient::unparsed(std::string_view operation, Kind kind,
                                   const Result& result) -> GraphqlError {
  return GraphqlError::replied(operation, kind, result.http_status,
                               result.attempts);
}

AuthenticatedClient::AuthenticatedClient(net::http::Client& client,
                                         auth::AppTokenSlot& credentials,
                                         Stats* stats) noexcept
    : sender_(client, stats), credentials_(credentials) {}

auto AuthenticatedClient::concept_by_id(std::string_view concept_id,
                                        std::string_view locale)
    -> Expected<TitleDetails, GraphqlError> {
  const auto& operation = authenticated::concept_by_id;
  auto sent = sender_.send_authenticated(operation, concept_endpoint,
                                         single_field("conceptId", concept_id),
                                         {{}, locale}, credentials_);
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<ConceptData>();
  auto& found = reply.concept_retrieve;
  auto details = found ? concept_details(*found) : std::nullopt;
  if (!details || details->concept_id() != concept_id)
    return unexpected(unparsed(operation.name,
                               found.null() ? Kind::not_found : Kind::malformed,
                               sent.value()));
  return std::move(*details);
}

auto AuthenticatedClient::concept_pricing(TitleDetails details,
                                          std::string_view locale)
    -> Expected<TitleDetails, GraphqlError> {
  const auto& operation = authenticated::concept_pricing;
  auto sent = sender_.send_authenticated(
      operation, pricing_endpoint,
      single_field("conceptId", details.concept_id()), {{}, locale},
      credentials_);
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<ConceptData>();
  auto& found = reply.concept_retrieve;
  auto merged = found ? priced(std::move(details), *found) : std::nullopt;
  if (!merged)
    return unexpected(unparsed(operation.name, Kind::malformed, sent.value()));
  return std::move(*merged);
}

auto AuthenticatedClient::concept_by_product(std::string_view product_id,
                                             std::string_view locale)
    -> Expected<TitleDetails, GraphqlError> {
  const auto& operation = authenticated::concept_by_product;
  auto sent = sender_.send_authenticated(operation, product_endpoint,
                                         single_field("productId", product_id),
                                         {{}, locale}, credentials_);
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<ProductData>();
  auto& found = reply.product_retrieve;
  auto details = found ? by_product(*found, product_id) : std::nullopt;
  if (!details)
    return unexpected(unparsed(
        operation.name, found.missing() ? Kind::not_found : Kind::malformed,
        sent.value()));
  return std::move(*details);
}

auto AuthenticatedClient::subscriptions()
    -> Expected<PlusSubscriptions, GraphqlError> {
  const auto& operation = authenticated::get_subscriptions;
  auto sent = sender_.send_authenticated(operation, subscriptions_endpoint,
                                         Variables::none(), {}, credentials_);
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<SubscriptionsData>();
  auto active = active_plus_subscriptions(reply);
  if (!active)
    return unexpected(unparsed(operation.name, Kind::malformed, sent.value()));
  return PlusSubscriptions{std::move(*active), sent.value().http_status,
                           sent.value().attempts};
}

auto AuthenticatedClient::active_plus_subscriptions(SubscriptionsData& data)
    -> std::optional<std::vector<ActivePlusSubscription>> {
  if (!data.fetch_subscriptions || !data.fetch_subscriptions->subscriptions)
    return std::nullopt;
  std::vector<ActivePlusSubscription> active_entries;
  for (auto& entry : *data.fetch_subscriptions->subscriptions) {
    if (!entry || !entry->subscription_type) return std::nullopt;
    if (*entry->subscription_type != "PS_PLUS") continue;
    if (!entry->active) return std::nullopt;
    if (!*entry->active) continue;
    if (!entry->tier_label || !entry->id) return std::nullopt;
    active_entries.push_back({
        .tier_label = std::move(*entry->tier_label),
        .sku_id = std::move(*entry->id),
        .product_name = std::move(entry->product_name).value_or(std::string{}),
        .end_date = std::move(entry->end_date).value_or(std::string{}),
    });
  }
  return active_entries;
}

}  // namespace libchiaking::psn::graphql
