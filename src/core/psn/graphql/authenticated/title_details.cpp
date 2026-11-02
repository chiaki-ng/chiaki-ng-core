#include "src/core/psn/graphql/authenticated/title_details.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>

namespace libchiaking::psn::graphql {

auto TitleDetails::of(Summary summary) -> std::optional<TitleDetails> {
  if (!valid_concept_id(summary.concept_id)) return std::nullopt;
  return TitleDetails{std::move(summary)};
}

auto TitleDetails::sku_base(std::string_view id) -> std::string_view {
  if (std::ranges::count(id, '-') != 3) return id;
  const auto dash = id.rfind('-');
  const auto suffix = id.substr(dash + 1);
  if (suffix.size() != 4 || suffix[0] < 'A' || suffix[0] > 'Z' ||
      !std::all_of(suffix.begin() + 1, suffix.end(), text::ascii_digit))
    return id;
  return id.substr(0, dash);
}

auto TitleDetails::valid_concept_id(std::string_view id) noexcept -> bool {
  return !id.empty() && id.size() <= concept_id_max_length &&
         std::ranges::all_of(id, text::ascii_digit);
}

void TitleDetails::add(TitleProduct product) {
  const auto existing =
      std::ranges::find(products_, product.id, &TitleProduct::id);
  if (existing != products_.end())
    merge(*existing, std::move(product));
  else
    products_.push_back(std::move(product));
}

auto TitleDetails::edition(std::string_view product_id)
    -> std::vector<TitleProduct>::iterator {
  return std::ranges::find(products_, sku_base(product_id), &TitleProduct::id);
}

auto TitleDetails::lists(std::string_view product_id) const -> bool {
  return std::ranges::contains(products_, sku_base(product_id),
                               &TitleProduct::id);
}

void TitleDetails::offer(std::string_view product_id,
                         std::vector<TitleOffer> offers) {
  const auto found = edition(product_id);
  if (found != products_.end() && !offers.empty())
    found->offers = std::move(offers);
}

void TitleDetails::settle(Ownership ownership) {
  ownership_ = std::move(ownership);
  for (auto& product : products_) product.owned = owns(product);
  priced_ = true;
}

void TitleDetails::fill_media(TitleMedia& target, TitleMedia&& source) {
  fill(target.key_art, std::move(source.key_art));
  fill(target.logo, std::move(source.logo));
  fill(target.cover, std::move(source.cover));
  fill(target.preview_video, std::move(source.preview_video));
  fill(target.screenshots, std::move(source.screenshots));
}

void TitleDetails::merge(TitleProduct& target, TitleProduct&& source) {
  fill(target.name, std::move(source.name));
  fill(target.np_title_id, std::move(source.np_title_id));
  fill(target.platforms, std::move(source.platforms));
  fill(target.release_date, std::move(source.release_date));
  fill(target.classification, std::move(source.classification));
  fill(target.edition, std::move(source.edition));
  if (!target.streaming.notices) target.streaming = std::move(source.streaming);
  fill(target.streaming_notice, std::move(source.streaming_notice));
  if (!source.offers.empty()) target.offers = std::move(source.offers);
  fill_media(target.media, std::move(source.media));
}

auto TitleDetails::owns(const TitleProduct& product) const -> bool {
  const auto matches = [&](const std::vector<std::string>& owned) {
    return std::ranges::any_of(owned, [&](const std::string& id) {
      return sku_base(id) == product.id ||
             (!product.np_title_id.empty() && id == product.np_title_id);
    });
  };
  return matches(ownership_.owned) || matches(ownership_.purchased);
}

}  // namespace libchiaking::psn::graphql
