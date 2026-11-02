#include "libchiaking/detail/consoles/firmware.hpp"
#include "src/core/utils/text/integer.hpp"

namespace libchiaking::consoles {

auto Firmware::from_system_version(Generation generation,
                                   std::string_view system_version) noexcept
    -> Firmware {
  if (generation == Generation::ps5) return Firmware{Family::ps5_1};
  const auto version = text::parse_integer<int>(system_version).value_or(0);
  if (version >= ps4_10_first_system_version || version <= 0)
    return Firmware{Family::ps4_10};
  return Firmware{version >= ps4_9_first_system_version ? Family::ps4_9
                                                        : Family::ps4_8};
}

}  // namespace libchiaking::consoles
