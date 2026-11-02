#include "libchiaking/detail/consoles/account_console.hpp"

#include "libchiaking/detail/consoles/console_name.hpp"

namespace libchiaking::consoles {

auto AccountConsole::create(DeviceUid device_uid, std::string name,
                            Generation generation, bool remote_play_enabled)
    -> std::optional<AccountConsole> {
  if (name.size() > name_limit || !ConsoleName::printable(name) ||
      (device_uid.is_main_ps4() && generation != Generation::ps4))
    return std::nullopt;
  return AccountConsole{device_uid, std::move(name), generation,
                        remote_play_enabled};
}

auto AccountConsole::main_ps4() -> AccountConsole {
  return AccountConsole{DeviceUid::main_ps4(), std::string{main_ps4_name},
                        Generation::ps4, true};
}

}  // namespace libchiaking::consoles
