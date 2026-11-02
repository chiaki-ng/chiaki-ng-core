#include "src/core/catalog/adapters/ps/store_listing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

namespace libchiaking::catalog::ps {

auto StoreListing::scalar_text(const std::optional<glz::raw_json>& stated)
    -> std::optional<std::string> {
  if (!stated) return std::nullopt;
  if (!stated->str.starts_with('"')) return stated->str;
  const auto text = json::ps::read_reply<json::ps::Text>(stated->str);
  return std::string{text ? text.value().view() : std::string_view{}};
}

auto StoreListing::decided(const std::optional<glz::raw_json>& stated)
    -> std::optional<bool> {
  if (!stated) return std::nullopt;
  return stated->str == "true";
}

auto StoreListing::concept_of(const std::optional<glz::raw_json>& stated)
    -> std::optional<ConceptId> {
  if (!stated) return std::nullopt;
  if (stated->str.starts_with('"')) {
    const auto text = json::ps::read_reply<json::ps::Text>(stated->str);
    return ConceptId::parse(text ? text.value().view() : std::string_view{});
  }
  const auto number = json::ps::Number::from_token(stated->str);
  return number ? concept_numbered(*number) : std::nullopt;
}

auto StoreListing::concept_numbered(const json::ps::Number& number)
    -> std::optional<ConceptId> {
  constexpr auto largest = std::numeric_limits<std::int64_t>::max();
  if (const auto* whole = std::get_if<std::uint64_t>(&number.value())) {
    const auto clamped =
        std::min<std::uint64_t>(*whole, static_cast<std::uint64_t>(largest));
    return clamped > 0 ? ConceptId::of(clamped) : std::nullopt;
  }
  if (const auto* whole = std::get_if<std::int64_t>(&number.value()))
    return *whole > 0 ? ConceptId::of(static_cast<std::uint64_t>(*whole))
                      : std::nullopt;
  const auto real = std::get<double>(number.value());
  if (std::isnan(real) || real < 1.0) return std::nullopt;
  return ConceptId::of(real >= static_cast<double>(largest)
                           ? static_cast<std::uint64_t>(largest)
                           : static_cast<std::uint64_t>(real));
}

auto StoreListing::names(const StoreNames& listed, std::string_view wanted)
    -> bool {
  return listed &&
         std::ranges::any_of(*listed, [wanted](const json::ps::Text& value) {
           return value.view() == wanted;
         });
}

auto StoreListing::image_of(const ImageList& images, std::int64_t first,
                            std::int64_t second) -> std::string_view {
  if (!images) return {};
  for (const auto& image : *images) {
    if (!image) continue;
    const auto type =
        image->type ? image->type->integer().value_or(0) : std::int64_t{};
    if (type != first && type != second) continue;
    if (const auto url = image->url.view(); !url.empty()) return url;
  }
  return {};
}

auto StoreListing::cover(const ImageList& images, std::string_view stated)
    -> std::string {
  auto url = image_of(images, preferred_cover, preferred_cover);
  if (url.empty()) url = image_of(images, fallback_cover, other_fallback_cover);
  if (url.empty()) url = stated;
  return std::string{url.substr(0, image_url_limit)};
}

auto StoreListing::listed_on(const ListedProduct& product, Shelf shelf)
    -> std::optional<ListedEntry> {
  const bool in_container =
      shelf == Shelf::old_service || shelf == Shelf::public_store;
  auto id = ProductId::parse(in_container ? product.id.view()
                                          : product.product_id.view());
  if (!id) return std::nullopt;
  ListedEntry entry{std::move(*id)};
  entry.name = product.name.view();
  entry.shelf = shelf;
  entry.concept_id = concept_of(product.concept_id);
  entry.on_ps5 = names(product.device, "PS5");
  entry.on_ps4 = names(product.device, "PS4");
  entry.streaming_supported = decided(product.streaming_supported);
  const auto stated = product.image_url.view();
  entry.cover_url = in_container || stated.empty()
                        ? cover(product.images, stated)
                        : std::string{stated};
  entry.landscape_cover_url = product.landscape_image_url.view();
  entry.concept_page = StorePage::at(product.concept_url.view(),
                                     StorePage::Source::concept_page);
  return entry;
}

auto StoreListing::row_of(const glz::raw_json& link) -> ContainerRow {
  return json::ps::read_reply<ContainerRow>(link.str).value_or(ContainerRow{});
}

auto StoreListing::listed_on(const glz::raw_json& link, Shelf shelf)
    -> std::optional<ListedEntry> {
  const auto product = json::ps::read_reply<ListedProduct>(link.str);
  return product ? listed_on(product.value(), shelf) : std::nullopt;
}

}  // namespace libchiaking::catalog::ps
