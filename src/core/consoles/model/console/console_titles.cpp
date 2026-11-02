#include "libchiaking/detail/consoles/console_titles.hpp"

#include "libchiaking/detail/consoles/console_name.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

InstalledTitle::InstalledTitle(Facts facts) noexcept
    : facts_(std::move(facts)) {}

auto InstalledTitle::create(Facts facts) -> std::optional<InstalledTitle> {
  if (!valid_id(facts.title_id) || !valid_id(facts.start_id) ||
      text::trim(facts.name).empty() || text::trim(facts.cover_url).empty() ||
      !valid_concept(facts.concept_id))
    return std::nullopt;
  return InstalledTitle{std::move(facts)};
}

auto InstalledTitle::valid_concept(std::string_view digits) noexcept -> bool {
  return digits.size() <= concept_limit &&
         std::ranges::all_of(digits, text::ascii_digit);
}

auto InstalledTitle::valid_id(std::string_view id) noexcept -> bool {
  return !id.empty() && id.size() <= id_limit && ConsoleName::printable(id);
}

}  // namespace libchiaking::consoles
