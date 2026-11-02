#include "libchiaking/detail/trophies/ids.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <limits>

namespace libchiaking::trophies {

auto NpCommunicationId::Spelled::admits(std::string_view text) noexcept
    -> bool {
  constexpr std::size_t letters = 4;
  constexpr std::size_t digits = 5;
  constexpr std::size_t suffix = 2;
  if (text.size() != letters + digits + 1 + suffix) return false;
  return std::ranges::all_of(text.substr(0, letters),
                             text::ascii_upper_letter) &&
         std::ranges::all_of(text.substr(letters, digits), text::ascii_digit) &&
         text[letters + digits] == '_' &&
         std::ranges::all_of(text.substr(letters + digits + 1),
                             text::ascii_digit);
}

auto NpCommunicationId::parse(std::string_view text)
    -> std::optional<NpCommunicationId> {
  auto admitted = Text::parse(text);
  if (!admitted) return std::nullopt;
  return NpCommunicationId{std::move(*admitted)};
}

auto TrophyGroupId::Spelled::admits(std::string_view text) noexcept -> bool {
  const auto allowed = [](char value) {
    return text::ascii_digit(value) || text::ascii_lower_letter(value);
  };
  return !text.empty() && text.size() <= text_limit &&
         std::ranges::all_of(text, allowed);
}

auto TrophyGroupId::parse(std::string_view text)
    -> std::optional<TrophyGroupId> {
  auto admitted = Text::parse(text);
  if (!admitted) return std::nullopt;
  return TrophyGroupId{std::move(*admitted)};
}

auto TrophyGroupId::base_game() -> TrophyGroupId {
  return TrophyGroupId{*Text::parse("default")};
}

auto TrophyId::of(std::int64_t number) -> std::optional<TrophyId> {
  if (number < 0 || number > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return TrophyId{static_cast<std::uint32_t>(number)};
}

}  // namespace libchiaking::trophies
