#include "libchiaking/detail/catalog/title_number.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <vector>

namespace libchiaking::catalog {

auto TitleNumber::Prefixed::admits(std::string_view text) noexcept -> bool {
  return text.size() > ps5_prefix.size() &&
         (text.starts_with(ps5_prefix) || text.starts_with(ps4_prefix)) &&
         std::ranges::all_of(text.substr(ps5_prefix.size()), text::ascii_digit);
}

auto TitleNumber::from(std::string_view id) -> std::optional<TitleNumber> {
  for (std::size_t position = 0; position + ps5_prefix.size() < id.size();
       ++position) {
    const auto rest = id.substr(position);
    const auto tail = rest.substr(ps5_prefix.size());
    const auto digits =
        std::min(tail.find_first_not_of("0123456789"), tail.size());
    if (auto admitted = Text::parse(rest.substr(0, ps5_prefix.size() + digits)))
      return TitleNumber{std::move(*admitted)};
  }
  return std::nullopt;
}

auto TitleNumber::platform_marked_in(std::string_view text) noexcept
    -> std::optional<Platform> {
  if (text.find(ps5_prefix) != std::string_view::npos) return Platform::ps5;
  if (text.find(ps4_prefix) != std::string_view::npos) return Platform::ps4;
  return std::nullopt;
}

auto TitleNumber::platform() const noexcept -> Platform {
  return text().starts_with(ps5_prefix) ? Platform::ps5 : Platform::ps4;
}

auto TitleKey::Bounded::admits(std::string_view text) noexcept -> bool {
  return !text.empty() && text.size() <= text_limit;
}

auto TitleKey::of(std::string_view id) -> std::optional<TitleKey> {
  constexpr std::string_view separators = "-_";
  id = text::c_string(id);
  if (id.empty()) return std::nullopt;
  if (const auto number = TitleNumber::from(id))
    return TitleKey{
        *Text::parse(std::string_view{number->text()}.substr(0, text_limit))};
  const auto input = id.substr(0, input_limit);
  std::vector<std::string_view> labels;
  auto start = input.find_first_not_of(separators);
  while (start != std::string_view::npos && labels.size() < label_limit) {
    const auto end =
        std::min(input.find_first_of(separators, start), input.size());
    labels.push_back(input.substr(start, std::min(end - start, label_bytes)));
    start = input.find_first_not_of(separators, end);
  }
  if (labels.size() < 2) return std::nullopt;
  labels.pop_back();
  std::string key;
  for (const auto label : labels) {
    if (!key.empty()) key += '|';
    key += label;
  }
  if (key.size() > text_limit) key.resize(text_limit);
  return TitleKey{*Text::parse(key)};
}

}  // namespace libchiaking::catalog
