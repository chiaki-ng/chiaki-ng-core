#include "src/core/consoles/adapters/roster/console_facts.hpp"

#include "src/core/consoles/adapters/roster/console_core.hpp"
#include "src/core/consoles/adapters/roster/roster.hpp"
#include "src/core/consoles/known_consoles_core.hpp"

#include <utility>

namespace libchiaking::consoles {

ConsoleFacts::ConsoleFacts(KnownConsoles& consoles) noexcept
    : roster_(consoles.core().roster()) {}

auto ConsoleFacts::sighted(Sighting sighting) -> std::shared_ptr<Console> {
  auto console =
      roster_.admit(sighting.hardware_id, sighting.firmware, sighting.name);
  recognised(*console, std::move(sighting));
  return console;
}

void ConsoleFacts::recognised(Console& console,
                              std::optional<Sighting> sighting) {
  if (sighting &&
      console.core().seen(
          sighting->firmware, sighting->name, sighting->address,
          sighting->in_rest_mode ? Presence::rest_mode : Presence::ready,
          std::move(sighting->running_title)))
    console.core().keep_facts();
}

void ConsoleFacts::out_of_sight(Console& console, bool search_still_looking) {
  console.core().lost(search_still_looking ? Presence::unknown
                                           : Presence::not_nearby);
}

void ConsoleFacts::named(Console& console, const ConsoleName& name) {
  if (console.core().learn(name)) console.core().keep_facts();
}

auto ConsoleFacts::paired(const users::PsUser& as, PairedConsole paired,
                          const std::optional<Host>& at,
                          std::optional<DeviceUid> device_uid)
    -> Expected<std::shared_ptr<Console>, ConsoleStore::Error> {
  return roster_.keep(as, std::move(paired), at, device_uid);
}

auto ConsoleFacts::linked(Console& console, const users::PsUser& user,
                          DeviceUid device_uid)
    -> Expected<void, Console::Unchanged> {
  return console.core().link(user.account_id(), device_uid);
}

auto ConsoleFacts::keys_of(const Console& console,
                           const users::PsUser& user) const
    -> std::optional<PairingKeys> {
  return console.core().keys_of(user.account_id());
}

auto ConsoleFacts::device_uid_of(const Console& console,
                                 const users::PsUser& user) const
    -> std::optional<DeviceUid> {
  return console.core().device_uid_of(user.account_id());
}

}  // namespace libchiaking::consoles
