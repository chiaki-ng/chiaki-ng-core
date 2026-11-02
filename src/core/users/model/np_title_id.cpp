#include "libchiaking/detail/users/np_title_id.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <cstddef>

namespace libchiaking {

auto NpTitleId::Spelled::admits(std::string_view text) noexcept -> bool {
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

auto NpTitleId::parse(std::string_view text) -> std::optional<NpTitleId> {
  auto admitted = Text::parse(text);
  if (!admitted) return std::nullopt;
  return NpTitleId{std::move(*admitted)};
}

}  // namespace libchiaking
