#include "src/core/psn/graphql/public_web/client.hpp"

#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::graphql {

auto PublicWebClient::ps_plus_included(const GridPrice& price) -> bool {
  if (price.service_branding.contains(ps_plus)) return true;
  if (!price.upsell_service_branding.contains(ps_plus) || !price.upsell_text)
    return false;
  return std::ranges::find(included_upsell_texts,
                           std::string_view{*price.upsell_text}) !=
         included_upsell_texts.end();
}

auto PublicWebClient::grid_page(GridData& data) -> std::optional<GridPage> {
  if (!data.category_grid_retrieve) return std::nullopt;
  auto& grid = *data.category_grid_retrieve;
  GridPage page;
  if (grid.page_info)
    page.total_count = grid.page_info->total_count.value_or(0);
  if (!grid.products) return page;
  for (auto& product : *grid.products) {
    if (!product) continue;
    const auto& price = product->price;
    GridProduct listed{std::move(product->id).value_or(std::string{}),
                       price && ps_plus_included(*price)};
    if (price && price->upsell_service_branding.contains(ps_plus))
      listed.upsell_text = price->upsell_text.value_or(std::string{});
    page.products.push_back(std::move(listed));
  }
  return page;
}

auto PublicWebClient::unreadable(std::string_view operation,
                                 const Result& result) -> GraphqlError {
  return GraphqlError::replied(operation, Kind::malformed, result.http_status,
                               result.attempts);
}

PublicWebClient::PublicWebClient(net::http::Client& client,
                                 Stats* stats) noexcept
    : sender_(client, stats) {}

auto PublicWebClient::store_category_grid(std::string_view category_id,
                                          int offset, int size,
                                          std::string_view locale)
    -> Expected<GridPage, GraphqlError> {
  const auto variables =
      Variables::of(GridAsked{.id = text::c_string(category_id),
                              .page_args = {.size = size, .offset = offset}});
  const auto& operation = public_web::category_grid_retrieve;
  auto sent =
      sender_.send_public(operation, grid_endpoint, variables,
                          {legacy_cloud_stream_http_user_agent, locale});
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<GridData>();
  auto page = grid_page(reply);
  if (!page) return unexpected(unreadable(operation.name, sent.value()));
  return std::move(*page);
}

auto PublicWebClient::star_rating_of(Stars& rated)
    -> std::optional<StarRating> {
  const auto total = rated.total_ratings_count.integer();
  if (!rated.average_rating || !total) return std::nullopt;
  StarRating rating{
      .average = *rated.average_rating,
      .average_text =
          std::move(rated.average_rating_for_display).value_or(std::string{}),
      .ratings = std::max<std::int64_t>(*total, 0),
      .percent_by_stars = {}};
  if (!rated.ratings_distribution) return rating;
  for (const auto& entry : *rated.ratings_distribution) {
    if (!entry || !entry->rating.present() || !entry->percentage_raw.present())
      continue;
    const auto index = entry->rating.value_or(0) - 1;
    if (index >= 0 && index < static_cast<int>(StarRating::most_stars))
      rating.percent_by_stars[static_cast<std::size_t>(index)] =
          std::clamp(entry->percentage_raw.value_or(0), 0, 100);
  }
  return rating;
}

auto PublicWebClient::rated(Tolerant<ContentRating>& rating)
    -> std::optional<ContentRating> {
  if (!rating || (rating->name.empty() && rating->description.empty()))
    return std::nullopt;
  return std::move(*rating);
}

auto PublicWebClient::concept_named(const Operation& operation,
                                    const net::http::Endpoint& endpoint,
                                    std::string_view concept_id,
                                    std::string_view locale)
    -> Expected<Concept, GraphqlError> {
  const auto variables =
      Variables::of(ConceptAsked{text::c_string(concept_id)});
  auto sent =
      sender_.send_public(operation, endpoint, variables,
                          {legacy_cloud_stream_http_user_agent, locale});
  if (!sent) return unexpected(std::move(sent.error()));
  auto reply = sent.value().reply<ConceptData>();
  auto& found = reply.concept_retrieve;
  if (!found || found->id.value_or(std::string{}) != concept_id)
    return unexpected(GraphqlError::replied(
        operation.name, found.missing() ? Kind::not_found : Kind::malformed,
        sent.value().http_status, sent.value().attempts));
  return std::move(*found);
}

auto PublicWebClient::concept_media(std::string_view concept_id,
                                    std::string_view locale)
    -> Expected<ProductMedia, GraphqlError> {
  auto grouped = concept_named(public_web::concept_media,
                               concept_media_endpoint, concept_id, locale);
  if (!grouped) return unexpected(std::move(grouped.error()));
  ProductMedia media{.product_id = {},
                     .concept_id = std::string{concept_id},
                     .of_product = {},
                     .of_concept = std::move(grouped->media).entries()};
  if (auto& product = grouped->default_product) {
    media.product_id = std::move(product->id).value_or(std::string{});
    media.of_product = std::move(product->media).entries();
  }
  return media;
}

auto PublicWebClient::concept_star_rating(std::string_view concept_id,
                                          std::string_view locale)
    -> Expected<std::optional<StarRating>, GraphqlError> {
  auto grouped =
      concept_named(public_web::concept_star_rating,
                    concept_star_rating_endpoint, concept_id, locale);
  if (!grouped) return unexpected(std::move(grouped.error()));
  auto& product = grouped->default_product;
  if (!product || !product->star_rating) return std::optional<StarRating>{};
  return star_rating_of(*product->star_rating);
}

auto PublicWebClient::concept_content_rating(std::string_view concept_id,
                                             std::string_view locale)
    -> Expected<std::optional<ContentRating>, GraphqlError> {
  auto grouped =
      concept_named(public_web::concept_content_rating,
                    concept_content_rating_endpoint, concept_id, locale);
  if (!grouped) return unexpected(std::move(grouped.error()));
  auto rating = rated(grouped->content_rating);
  auto& product = grouped->default_product;
  if (!rating && product) rating = rated(product->content_rating);
  return rating;
}

auto PublicWebClient::concept_info(std::string_view concept_id,
                                   std::string_view locale)
    -> Expected<ProductInfo, GraphqlError> {
  auto grouped = concept_named(public_web::concept_game_info,
                               concept_info_endpoint, concept_id, locale);
  if (!grouped) return unexpected(std::move(grouped.error()));
  ProductInfo info{
      .publisher = std::move(grouped->publisher_name).value_or(std::string{}),
      .release_date = std::move(grouped->release_date).text(),
      .genres = std::move(grouped->localized_genres).names()};
  if (auto& product = grouped->default_product) {
    if (info.publisher.empty())
      info.publisher =
          std::move(product->publisher_name).value_or(std::string{});
    if (info.release_date.empty())
      info.release_date = std::move(product->release_date).text();
    if (info.genres.empty())
      info.genres = std::move(product->localized_genres).names();
  }
  return info;
}

}  // namespace libchiaking::psn::graphql
