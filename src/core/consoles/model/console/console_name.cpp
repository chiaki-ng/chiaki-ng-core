#include "libchiaking/detail/consoles/console_name.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>

namespace libchiaking::consoles {

auto ConsoleName::create(std::string text, Source source)
    -> std::optional<ConsoleName> {
  if (source == Source::generation || text.size() > limit ||
      text::trim(text).empty() || !printable(text))
    return std::nullopt;
  return ConsoleName{std::move(text), source};
}

auto ConsoleName::of(Generation generation) -> ConsoleName {
  return ConsoleName{
      std::string{generation == Generation::ps5 ? ps5_label : ps4_label},
      Source::generation};
}

auto ConsoleName::printable(std::string_view text) noexcept -> bool {
  constexpr unsigned char first_visible = 0x20;
  constexpr unsigned char delete_character = 0x7f;
  return std::ranges::none_of(text, [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte < first_visible || byte == delete_character;
  });
}

}  // namespace libchiaking::consoles
