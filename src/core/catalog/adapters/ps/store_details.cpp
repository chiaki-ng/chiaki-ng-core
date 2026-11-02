#include "src/core/catalog/adapters/ps/store_details.hpp"

#include "src/core/catalog/adapters/ps/streaming_category.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/rest/store/store_client.hpp"

#include <libchiaking/detail/catalog/release_date.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

StoreDetails::StoreDetails(net::http::Client& http,
                           platform::ThreadingProvider& threading,
                           const platform::ClockProvider& wall) noexcept
    : http_(http), threading_(threading), stamp_(wall) {}

auto StoreDetails::images_in(Media media, std::string_view role)
    -> std::vector<ImageRef> {
  std::vector<ImageRef> found;
  for (const auto& item : media) {
    if (item.role != role || item.type != image_type) continue;
    if (auto image = ImageRef::at(item.url)) found.push_back(std::move(*image));
  }
  return found;
}

auto StoreDetails::trailers_in(Media media) -> std::vector<TrailerRef> {
  std::vector<TrailerRef> found;
  for (const auto& item : media) {
    if (item.role != trailer_role || item.type != video_type) continue;
    if (auto trailer = TrailerRef::at(item.url))
      found.push_back(std::move(*trailer));
  }
  return found;
}

auto StoreDetails::image_in(Media media, std::string_view role)
    -> std::optional<ImageRef> {
  auto found = images_in(media, role);
  if (found.empty()) return std::nullopt;
  return std::move(found.front());
}

auto StoreDetails::concept_first(Media of_product, Media of_concept,
                                 std::string_view role)
    -> std::optional<ImageRef> {
  auto found = image_in(of_concept, role);
  if (!found) found = image_in(of_product, role);
  return found;
}

auto StoreDetails::key_art_of(Media of_product, Media of_concept)
    -> std::optional<KeyArt> {
  auto logo = concept_first(of_product, of_concept, logo_role);
  auto layered = LayeredArt::of(image_in(of_concept, background_role),
                                image_in(of_concept, hero_role), logo);
  return KeyArt::preferring_layers(
      std::move(layered),
      GameHubArt::of(concept_first(of_product, of_concept, game_hub_role),
                     std::move(logo)));
}

void StoreDetails::show_media(GameDetails& details, Media of_product,
                              Media of_concept) {
  details.key_art = key_art_of(of_product, of_concept);
  details.screenshots = images_in(of_product, screenshot_role);
  if (details.screenshots.empty())
    details.screenshots = images_in(of_concept, screenshot_role);
  details.trailers = trailers_in(of_product);
  if (details.trailers.empty()) details.trailers = trailers_in(of_concept);
}

auto StoreDetails::media_in(const Tolerant<Gallery>& owner)
    -> std::vector<psn::graphql::StoreMedia> {
  std::vector<psn::graphql::StoreMedia> found;
  if (!owner) return found;
  for (const auto& [items, type] : {std::pair{&owner->images, image_type},
                                    std::pair{&owner->videos, video_type}}) {
    if (!*items) continue;
    for (const auto& shown : **items)
      found.push_back(
          {.role = std::string{shown ? shown->type.view() : std::string_view{}},
           .type = std::string{type},
           .url = std::string{shown ? shown->url.view() : std::string_view{}}});
  }
  return found;
}

auto StoreDetails::star_rating_in(const Sold& product)
    -> std::optional<StarRating> {
  const auto& rated = product.star_rating;
  if (!rated || !rated->count) return std::nullopt;
  const auto integer = [](const Tolerant<json::ps::Number>& number) {
    return number ? number->integer().value_or(0) : std::int64_t{};
  };
  std::array<std::int64_t, StarRating::most_stars> by_stars{};
  for (const auto& vote : *rated->count) {
    if (!vote) continue;
    const auto stars = integer(vote->score);
    if (stars >= 1 &&
        stars <= static_cast<std::int64_t>(StarRating::most_stars))
      by_stars[static_cast<std::size_t>(stars - 1)] =
          std::max<std::int64_t>(integer(vote->count), 0);
  }
  StarRating rating{.average = {},
                    .average_text = std::string{rated->score.view()},
                    .ratings = {},
                    .percent_by_stars = {}};
  std::int64_t stars_given = 0;
  for (std::size_t index = 0; index < by_stars.size(); ++index) {
    rating.ratings += by_stars[index];
    stars_given += by_stars[index] * static_cast<std::int64_t>(index + 1);
  }
  if (rating.ratings == 0) return std::nullopt;
  rating.average =
      static_cast<double>(stars_given) / static_cast<double>(rating.ratings);
  for (std::size_t index = 0; index < by_stars.size(); ++index)
    rating.percent_by_stars[index] = static_cast<int>(
        (by_stars[index] * 100 + rating.ratings / 2) / rating.ratings);
  return rating;
}

auto StoreDetails::age_rating_in(const Sold& product)
    -> std::optional<AgeRating> {
  const auto& rated = product.content_rating;
  if (!rated) return std::nullopt;
  AgeRating rating{std::string{rated->authority.view()},
                   std::string{rated->name.view()},
                   std::string{rated->description.view()},
                   {},
                   {}};
  if (rating.name.empty() && rating.description.empty()) return std::nullopt;
  const auto sharp = rated->url4k.view();
  rating.icon = ImageRef::at(sharp.empty() ? rated->url.view() : sharp);
  if (rated->descriptors)
    for (const auto& reason : *rated->descriptors)
      if (const auto text =
              reason ? reason->description.view() : std::string_view{};
          !text.empty())
        rating.descriptors.emplace_back(text);
  return rating;
}

auto StoreDetails::genres_in(const Sold& product) -> std::vector<std::string> {
  std::vector<std::string> genres;
  if (product.localized_genres)
    for (const auto& genre : *product.localized_genres)
      if (const auto name = genre ? genre->value.view() : std::string_view{};
          !name.empty())
        genres.emplace_back(name);
  return genres;
}

auto StoreDetails::star_rating_of(const psn::graphql::StarRating& rating)
    -> StarRating {
  return StarRating{rating.average, rating.average_text, rating.ratings,
                    rating.percent_by_stars};
}

auto StoreDetails::age_rating_of(const psn::graphql::ContentRating& rating)
    -> AgeRating {
  return AgeRating{rating.authority, rating.name, rating.description,
                   ImageRef::at(rating.icon_url), rating.descriptors};
}

auto StoreDetails::gave_up(const psn::graphql::GraphqlError& error) noexcept
    -> bool {
  return error.kind() == psn::graphql::GraphqlError::Kind::cancelled;
}

auto StoreDetails::unanswered(const psn::graphql::GraphqlError& error) noexcept
    -> Unanswered {
  using Kind = psn::graphql::GraphqlError::Kind;
  using Reason = Unanswered::Reason;
  const auto status = error.http_status();
  switch (error.kind()) {
    case Kind::cancelled:
      return Unanswered::cancelled();
    case Kind::invalid_json:
    case Kind::malformed:
      return Unanswered{Reason::malformed, status};
    case Kind::not_found:
      return Unanswered{Reason::not_listed, status};
    case Kind::auth_refused:
    case Kind::invalid_request:
    case Kind::errors:
    case Kind::partial:
    case Kind::rejected:
    case Kind::missing_credential:
      return Unanswered{Reason::rejected, status};
    case Kind::transport:
    case Kind::server_error:
      break;
  }
  return Unanswered{Reason::unavailable, status};
}

auto StoreDetails::of_product(const ProductId& product,
                              const StoreLocale& locale, int age)
    -> Expected<GameDetails, Unanswered> {
  const auto reply =
      psn::rest::StoreClient{http_}.products<std::vector<Tolerant<Sold>>>(
          {.product_ids = product.text(),
           .country = locale.country(),
           .language = locale.language(),
           .age = age,
           .with_concept = true});
  if (!reply) return unexpected(StreamingCategory::unanswered(reply.error()));
  const auto sold = std::ranges::find_if(
      reply.value(), [&product](const Tolerant<Sold>& entry) {
        return entry && entry->id.view() == product.text();
      });
  if (sold == reply.value().end() || (*sold)->error_code)
    return unexpected(
        Unanswered{Unanswered::Reason::not_listed, net::HttpStatus::ok.code()});
  const auto& listed = **sold;
  const auto& grouped = listed.grouped;
  GameDetails details{
      .product = product, .locale = locale, .fetched_at = stamp_.now()};
  show_media(details, media_in(listed.media),
             grouped ? media_in(grouped->media)
                     : std::vector<psn::graphql::StoreMedia>{});
  details.star_rating = star_rating_in(listed);
  details.age_rating = age_rating_in(listed);
  details.publisher = std::string{listed.lead_publisher_name.view()};
  if (details.publisher.empty() && grouped)
    details.publisher = std::string{grouped->publisher_name.view()};
  details.released_at = ReleaseDate::parse_time(listed.release_date.view());
  details.genres = genres_in(listed);
  return details;
}

auto StoreDetails::of_concept(const ConceptId& grouped_under,
                              const StoreLocale& locale)
    -> Expected<Found, Unanswered> {
  using Store = psn::graphql::PublicWebClient;
  using Error = psn::graphql::GraphqlError;
  const auto store_locale = locale.text();
  const auto& id = grouped_under.text();
  std::optional<Expected<psn::graphql::ProductMedia, Error>> media;
  std::optional<Expected<std::optional<psn::graphql::StarRating>, Error>> stars;
  std::optional<Expected<std::optional<psn::graphql::ContentRating>, Error>>
      age;
  std::optional<Expected<psn::graphql::ProductInfo, Error>> info;
  const auto ask_media = [&] {
    media.emplace(Store{http_}.concept_media(id, store_locale));
  };
  const auto ask_stars = [&] {
    stars.emplace(Store{http_}.concept_star_rating(id, store_locale));
  };
  const auto ask_age = [&] {
    age.emplace(Store{http_}.concept_content_rating(id, store_locale));
  };
  const auto ask_info = [&] {
    info.emplace(Store{http_}.concept_info(id, store_locale));
  };
  {
    std::vector<platform::Thread> workers;
    const auto start = [&](auto& work) {
      auto started = platform::Thread::start(threading_, "chiaki-store",
                                             platform::ThreadRole::unspecified,
                                             std::ref(work));
      if (!started) return false;
      workers.push_back(std::move(started).value());
      return true;
    };
    const bool stars_asked = start(ask_stars);
    const bool age_asked = start(ask_age);
    const bool info_asked = start(ask_info);
    ask_media();
    if (!stars_asked) ask_stars();
    if (!age_asked) ask_age();
    if (!info_asked) ask_info();
  }
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  if (!*media) return unexpected(unanswered(media->error()));
  for (const auto* failure : {stars->has_value() ? nullptr : &stars->error(),
                              age->has_value() ? nullptr : &age->error(),
                              info->has_value() ? nullptr : &info->error()})
    if (failure != nullptr && gave_up(*failure))
      return unexpected(Unanswered::cancelled());

  Found found{GameDetails{.concept_id = grouped_under,
                          .locale = locale,
                          .fetched_at = stamp_.now()},
              stars->has_value() && age->has_value() && info->has_value()};
  auto& details = found.details;
  show_media(details, media->value().of_product, media->value().of_concept);
  if (*stars && stars->value())
    details.star_rating = star_rating_of(*stars->value());
  if (*age && age->value()) details.age_rating = age_rating_of(*age->value());
  if (*info) {
    details.publisher = info->value().publisher;
    details.released_at = ReleaseDate::parse_time(info->value().release_date);
    details.genres = info->value().genres;
  }
  return found;
}

auto StoreImages::bytes_of(const ImageRef& image)
    -> Expected<std::vector<std::byte>, Unanswered> {
  using Reason = Unanswered::Reason;
  auto sent = http_.send(net::http::Request{image_endpoint, image.url()});
  if (http_.stop_requested() || (!sent && sent.error().is_cancelled()))
    return unexpected(Unanswered::cancelled());
  if (!sent)
    return unexpected(Unanswered{sent.error().error() ==
                                         platform::HttpError::response_too_large
                                     ? Reason::rejected
                                     : Reason::unavailable});
  const auto status = sent.value().status();
  const auto bytes = sent.value().bytes();
  if (net::HttpStatus{status} == net::HttpStatus::not_found)
    return unexpected(Unanswered{Reason::not_listed, status});
  if (net::HttpStatus{status}.is_server_error())
    return unexpected(Unanswered{Reason::unavailable, status});
  if (net::HttpStatus{status} != net::HttpStatus::ok)
    return unexpected(Unanswered{Reason::rejected, status});
  if (bytes.empty()) return unexpected(Unanswered{Reason::malformed, status});
  if (bytes.size() > ImageHost::largest_bytes)
    return unexpected(Unanswered{Reason::rejected, status});
  return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

auto StorePages::page_of(const ProductId& product, const StoreLocale& locale,
                         const std::optional<StorePage>& concept_page)
    -> Expected<StorePage, Unanswered> {
  std::uint16_t refused_with = 0;
  if (const auto page = StorePage::of_product(locale, product)) {
    psn::rest::StoreClient store{http_};
    const auto exists = store.product_page_exists(page->url());
    if (exists && exists.value()) return *page;
    if (!exists) refused_with = exists.error().http_status();
  }
  if (concept_page) return *concept_page;
  if (http_.stop_requested()) return unexpected(Unanswered::cancelled());
  return unexpected(Unanswered{Unanswered::Reason::not_listed, refused_with});
}

}  // namespace libchiaking::catalog::ps
