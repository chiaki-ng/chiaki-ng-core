#include "src/core/utils/timing/instant.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <iterator>
#include <string>

namespace libchiaking::timing {

auto Instant::from_text(std::string_view text) -> std::optional<Instant> {
  if (!names_its_zone(text)) return std::nullopt;
  std::string quoted;
  quoted.reserve(text.size() + 2);
  quoted.push_back('"');
  std::ranges::transform(text, std::back_inserter(quoted), text::ascii_upper);
  quoted.push_back('"');
  Time time{};
  if (glz::read_json(time, quoted)) return std::nullopt;
  return Instant{time};
}

}  // namespace libchiaking::timing
