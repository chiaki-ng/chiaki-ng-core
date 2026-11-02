#include "libchiaking/detail/catalog/store_page.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

StorePage::StorePage(std::string url, Source source) noexcept
    : url_(std::move(url)), source_(source) {}

auto StorePage::at(std::string_view url, Source source)
    -> std::optional<StorePage> {
  if (url.size() <= origin.size() || url.size() > url_limit ||
      !url.starts_with(origin) || !std::ranges::all_of(url, url_character))
    return std::nullopt;
  return StorePage{std::string{url}, source};
}

auto StorePage::of_product(const StoreLocale& locale, const ProductId& product)
    -> std::optional<StorePage> {
  if (!store_product(product.text())) return std::nullopt;
  return at(
      std::string{origin} + locale.lowercase() + "/product/" + product.text(),
      Source::product);
}

auto StorePage::url_character(char value) noexcept -> bool {
  constexpr std::string_view extra = "-._~/?=&%:+";
  return text::ascii_alnum(value) ||
         extra.find(value) != std::string_view::npos;
}

auto StorePage::store_product(std::string_view id) noexcept -> bool {
  constexpr std::size_t length = 36;
  constexpr std::size_t region_length = 2;
  constexpr std::size_t publisher_end = 6;
  constexpr std::size_t title_prefix_length = 4;
  constexpr std::size_t title_end = 16;
  constexpr std::size_t version_end = 19;
  constexpr auto title_start = publisher_end + 1;
  constexpr auto title_number_start = title_start + title_prefix_length;
  constexpr auto version_start = title_end + 1;
  constexpr auto label_start = version_end + 1;
  const auto upper_alnum = [](char value) {
    return text::ascii_upper_letter(value) || text::ascii_digit(value);
  };
  if (id.size() != length || id[publisher_end] != '-' || id[title_end] != '_' ||
      id[version_end] != '-')
    return false;
  return std::ranges::all_of(id.substr(0, region_length), upper_alnum) &&
         std::ranges::all_of(
             id.substr(region_length, publisher_end - region_length),
             text::ascii_digit) &&
         std::ranges::all_of(id.substr(title_start, title_prefix_length),
                             upper_alnum) &&
         std::ranges::all_of(
             id.substr(title_number_start, title_end - title_number_start),
             text::ascii_digit) &&
         std::ranges::all_of(
             id.substr(version_start, version_end - version_start),
             text::ascii_digit) &&
         std::ranges::all_of(id.substr(label_start), upper_alnum);
}

}  // namespace libchiaking::catalog
