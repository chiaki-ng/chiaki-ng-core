#include "libchiaking/detail/play/datacenter.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>

namespace libchiaking::play {

auto DatacenterName::parse(std::string_view text)
    -> std::optional<DatacenterName> {
  if (text.empty() || text == automatic || text.size() > limit ||
      std::ranges::any_of(text, text::ascii_control))
    return std::nullopt;
  return DatacenterName{text};
}

}  // namespace libchiaking::play
