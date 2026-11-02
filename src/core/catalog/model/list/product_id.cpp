#include "libchiaking/detail/catalog/product_id.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

auto ProductId::Spelling::admits(std::string_view text) noexcept -> bool {
  const auto allowed = [](char value) {
    return text::ascii_alnum(value) || value == '-' || value == '_';
  };
  return !text.empty() && text.size() <= limit &&
         std::ranges::all_of(text, allowed);
}

auto ProductId::parse(std::string_view text) -> std::optional<ProductId> {
  auto admitted = Text::parse(text);
  if (!admitted) return std::nullopt;
  return ProductId{std::move(*admitted)};
}

auto ProductId::title_number() const -> std::optional<TitleNumber> {
  return TitleNumber::from(text());
}

auto ProductId::title_key() const -> std::optional<TitleKey> {
  return TitleKey::of(text());
}

auto ProductId::platform() const -> std::optional<Platform> {
  return TitleNumber::platform_marked_in(text());
}

auto ProductId::without_sku() const -> ProductId {
  const auto dash = sku_dash();
  return dash ? *parse(std::string_view{text()}.substr(0, *dash)) : *this;
}

auto ProductId::has_sku() const noexcept -> bool {
  return sku_dash().has_value();
}

auto ProductId::is_answered_by(
    const ProductId& storefront_product) const noexcept -> bool {
  const auto& asked = text();
  const auto& answered = storefront_product.text();
  if (asked == answered) return true;
  return asked.size() > answered.size() && asked.starts_with(answered) &&
         asked[answered.size()] == '-';
}

auto ProductId::sku_dash() const noexcept -> std::optional<std::size_t> {
  constexpr std::ptrdiff_t dashes_before_sku = 3;
  constexpr std::size_t sku_length = 4;
  const auto& id = text();
  if (std::ranges::count(id, '-') != dashes_before_sku) return std::nullopt;
  const auto dash = id.rfind('-');
  const auto sku = std::string_view{id}.substr(dash + 1);
  if (dash == 0 || sku.size() != sku_length || sku[0] < 'A' || sku[0] > 'Z' ||
      !std::all_of(sku.begin() + 1, sku.end(), text::ascii_digit))
    return std::nullopt;
  return dash;
}

}  // namespace libchiaking::catalog
