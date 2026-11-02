#include "libchiaking/detail/catalog/title.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

auto ConceptId::Digits::admits(std::string_view digits) noexcept -> bool {
  return !digits.empty() && digits.size() <= limit &&
         std::ranges::all_of(digits, text::ascii_digit) &&
         !std::ranges::all_of(digits, [](char digit) { return digit == '0'; });
}

auto ConceptId::parse(std::string_view digits) -> std::optional<ConceptId> {
  auto admitted = Text::parse(digits);
  if (!admitted) return std::nullopt;
  return ConceptId{std::move(*admitted)};
}

auto ConceptId::of(std::uint64_t number) -> std::optional<ConceptId> {
  return parse(std::to_string(number));
}

Title::Title(Facts facts) noexcept : facts_(std::move(facts)) {}

auto Title::from(Facts facts) -> Expected<Title, Invalid> {
  if (text::trim(facts.name).empty()) return unexpected(Invalid::name);
  std::vector<ProductId> aliases;
  for (auto& alias : facts.aliases)
    if (alias != facts.product &&
        std::ranges::find(aliases, alias) == aliases.end())
      aliases.push_back(std::move(alias));
  facts.aliases = std::move(aliases);
  return Title{std::move(facts)};
}

auto Title::title_number() const -> std::optional<TitleNumber> {
  return facts_.product.title_number();
}

auto Title::is_known_as(const ProductId& product) const noexcept -> bool {
  return facts_.product == product ||
         std::ranges::find(facts_.aliases, product) != facts_.aliases.end();
}

auto Title::with_streamability(ListStreamability streamability) const -> Title {
  auto facts = facts_;
  facts.streamability = streamability;
  return Title{std::move(facts)};
}

auto Title::with_alias(const ProductId& alias) const -> Title {
  auto facts = facts_;
  if (!is_known_as(alias)) facts.aliases.push_back(alias);
  return Title{std::move(facts)};
}

auto Title::without_alias(const ProductId& alias) const -> Title {
  auto facts = facts_;
  std::erase(facts.aliases, alias);
  return Title{std::move(facts)};
}

}  // namespace libchiaking::catalog
