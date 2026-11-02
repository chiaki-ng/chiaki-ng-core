#include "libchiaking/detail/catalog/store_locale.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace libchiaking::catalog {

StoreLocale::StoreLocale(std::string language, std::string country) noexcept
    : language_(std::move(language)), country_(std::move(country)) {}

auto StoreLocale::parse(std::string_view text) -> std::optional<StoreLocale> {
  text = text::trim(text);
  const auto dash = text.find_last_of("-_");
  if (dash == std::string_view::npos) return std::nullopt;
  auto language = language_of(text.substr(0, dash));
  auto country = country_of(text.substr(dash + 1));
  if (!language || !country) return std::nullopt;
  return StoreLocale{std::move(*language), std::move(*country)};
}

auto StoreLocale::for_account(const users::Region& region)
    -> std::optional<StoreLocale> {
  auto country = country_of(region.country);
  if (!country) return std::nullopt;
  const std::string_view spoken{region.language};
  auto language = language_of(spoken.substr(0, spoken.find_first_of("-_")));
  return StoreLocale{language ? std::move(*language) : std::string{"en"},
                     std::move(*country)};
}

auto StoreLocale::english_us() -> StoreLocale {
  return StoreLocale{"en", "US"};
}

auto StoreLocale::shared_store_country(std::string_view account_country)
    -> std::string_view {
  constexpr std::array<std::string_view, 19> americas{
      "US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "EC", "BO",
      "PY", "UY", "CR", "GT", "HN", "NI", "PA", "SV", "DO"};
  return std::ranges::any_of(americas,
                             [account_country](std::string_view listed) {
                               return text::ieq(listed, account_country);
                             })
             ? "US"
             : "GB";
}

auto StoreLocale::text() const -> std::string {
  return language_ + "-" + country_;
}

auto StoreLocale::lowercase() const -> std::string {
  auto lowered = text();
  std::ranges::transform(lowered, lowered.begin(), text::ascii_lower);
  return lowered;
}

auto StoreLocale::english() const -> StoreLocale {
  return StoreLocale{"en", country_};
}

auto StoreLocale::fallbacks() const -> std::vector<StoreLocale> {
  std::vector<StoreLocale> chain;
  for (auto candidate : {*this, english(), english_us()})
    if (std::ranges::find(chain, candidate) == chain.end())
      chain.push_back(std::move(candidate));
  return chain;
}

auto StoreLocale::is_in_country_of(const users::Region& region) const noexcept
    -> bool {
  return text::ieq(country_, region.country);
}

auto StoreLocale::language_of(std::string_view text)
    -> std::optional<std::string> {
  constexpr std::size_t shortest = 2;
  const auto allowed = [](char value) {
    return letter(value) || value == '-' || value == '_';
  };
  if (text.size() < shortest || text.size() > language_limit ||
      !letter(text.front()) || !letter(text.back()) ||
      !std::ranges::all_of(text, allowed))
    return std::nullopt;
  std::string language{text};
  std::ranges::transform(language, language.begin(), [](char value) {
    return value == '_' ? '-' : text::ascii_lower(value);
  });
  return language;
}

auto StoreLocale::country_of(std::string_view text)
    -> std::optional<std::string> {
  if (text.size() != country_length || !std::ranges::all_of(text, letter))
    return std::nullopt;
  std::string country{text};
  std::ranges::transform(country, country.begin(), text::ascii_upper);
  return country;
}

auto StoreLocale::letter(char value) noexcept -> bool {
  return text::ascii_alnum(value) && !text::ascii_digit(value);
}

}  // namespace libchiaking::catalog
