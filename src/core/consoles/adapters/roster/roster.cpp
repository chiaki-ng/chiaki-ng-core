#include "src/core/consoles/adapters/roster/roster.hpp"

#include "src/core/consoles/adapters/roster/console_core.hpp"
#include "src/core/consoles/known_consoles_core.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

Roster::Roster(KnownConsoles::Core& home, ConsoleStore& store,
               platform::DiagnosticsProvider* log)
    : home_(home),
      store_(store),
      log_(log),
      changes_(home.threading()),
      mutex_(home.threading()) {}

Roster::~Roster() {
  for (const auto& console : consoles_) console->core().leave();
}

auto Roster::made(SavedConsole facts, bool saved) const
    -> std::shared_ptr<Console> {
  return std::make_shared<Console>(std::make_unique<Console::Core>(
      home_, store_, log_, std::move(facts), saved));
}

auto Roster::remember_saved()
    -> Expected<KnownConsoles::Remembered, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  auto saved = store_.load_saved_consoles();
  if (!saved) return unexpected(saved.error());
  KnownConsoles::Remembered remembered;
  for (auto& row : saved.value().consoles) {
    const auto listed = std::ranges::find(remembered.consoles, row.hardware_id,
                                          &Console::hardware_id);
    if (listed != remembered.consoles.end() ||
        (row.last_address &&
         !row.last_address->reaches(row.firmware.generation()))) {
      ++remembered.rejected;
      continue;
    }
    auto present = find(row.hardware_id);
    if (present && present->generation() != row.firmware.generation()) {
      ++remembered.rejected;
      continue;
    }
    if (!present) {
      present = made(std::move(row), true);
      platform::Lock lock{mutex_};
      consoles_.push_back(present);
    } else {
      present->core().remembered(row.name);
    }
    remembered.consoles.push_back(std::move(present));
  }
  std::vector<std::pair<AccountId, HardwareId>> loaded;
  for (auto& row : saved.value().pairings) {
    const auto owner = std::ranges::find(remembered.consoles, row.console,
                                         &Console::hardware_id);
    const std::pair key{row.account, row.console};
    if (owner == remembered.consoles.end() || row.account.value() == 0 ||
        std::ranges::find(loaded, key) != loaded.end()) {
      ++remembered.rejected;
      continue;
    }
    loaded.push_back(key);
    if (!(*owner)->core().holds(row.account))
      (*owner)->core().pair(row.account, std::move(row.keys), row.device_uid);
  }
  return remembered;
}

auto Roster::held(const Console& console) const -> std::shared_ptr<Console> {
  platform::Lock lock{mutex_};
  const auto found = std::ranges::find_if(
      consoles_, [&console](const std::shared_ptr<Console>& known) {
        return known.get() == &console;
      });
  if (found == consoles_.end()) return nullptr;
  return *found;
}

auto Roster::forget(Console& console) -> Expected<void, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  const auto known = held(console);
  if (!known) return {};
  if (const auto forgotten = known->core().forget(); !forgotten)
    return forgotten;
  platform::Lock lock{mutex_};
  std::erase(consoles_, known);
  return {};
}

auto Roster::unpair(AccountId account, Console& console)
    -> Expected<void, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  const auto known = held(console);
  if (!known) return {};
  return known->core().unpair(account);
}

auto Roster::find(HardwareId hardware_id) const -> std::shared_ptr<Console> {
  platform::Lock lock{mutex_};
  const auto found =
      std::ranges::find(consoles_, hardware_id, &Console::hardware_id);
  return found != consoles_.end() ? *found : nullptr;
}

auto Roster::everything() const -> std::vector<std::shared_ptr<Console>> {
  platform::Lock lock{mutex_};
  return consoles_;
}

auto Roster::admit(HardwareId hardware_id, Firmware firmware,
                   const std::optional<ConsoleName>& name)
    -> std::shared_ptr<Console> {
  platform::Lock changes{changes_};
  if (auto present = find(hardware_id)) return present;
  auto admitted =
      made(SavedConsole{hardware_id, firmware,
                        name.value_or(ConsoleName::of(firmware.generation()))},
           false);
  platform::Lock lock{mutex_};
  consoles_.push_back(admitted);
  return admitted;
}

auto Roster::keep(const users::PsUser& as, PairedConsole paired,
                  const std::optional<Host>& at,
                  std::optional<DeviceUid> device_uid)
    -> Expected<std::shared_ptr<Console>, ConsoleStore::Error> {
  platform::Lock changes{changes_};
  auto present = find(paired.hardware_id);
  if (present && present->generation() != paired.firmware.generation()) {
    if (const auto forgotten = present->core().forget(); !forgotten)
      return unexpected(forgotten.error());
    platform::Lock lock{mutex_};
    std::erase(consoles_, present);
    present.reset();
  }
  const bool fresh = !present;
  auto console = fresh ? made(SavedConsole{paired.hardware_id, paired.firmware,
                                           paired.name.value_or(ConsoleName::of(
                                               paired.firmware.generation()))},
                              false)
                       : present;
  if (const auto kept = console->core().keep_pairing(
          as.account_id(), std::move(paired), at, device_uid);
      !kept)
    return unexpected(kept.error());
  if (fresh) {
    platform::Lock lock{mutex_};
    consoles_.push_back(console);
  }
  return console;
}

}  // namespace libchiaking::consoles
