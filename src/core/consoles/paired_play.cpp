#include "src/core/consoles/paired_play.hpp"

#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "src/core/consoles/adapters/pairing/account_link.hpp"
#include "src/core/consoles/adapters/roster/console_core.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::consoles {

PairedPlay::Reports::Reports(KnownConsoles& known,
                             std::shared_ptr<Console> console,
                             std::shared_ptr<const users::PsUser> as) noexcept
    : known_(known), console_(std::move(console)), as_(std::move(as)) {}

void PairedPlay::Reports::named(std::string_view name) {
  if (const auto learned =
          ConsoleName::create(std::string{name}, ConsoleName::Source::pairing))
    ConsoleFacts{known_}.named(*console_, *learned);
}

auto PairedPlay::Reports::paired(PairedConsole paired,
                                 const DeviceUid& device_uid)
    -> Expected<std::shared_ptr<Console>, ConsoleStore::Error> {
  return ConsoleFacts{known_}.paired(*as_, std::move(paired), std::nullopt,
                                     device_uid);
}

auto PairedPlay::Reports::recognises(const PairedConsole& reached) const
    -> bool {
  if (console_->generation() != Generation::ps4) return true;
  return std::ranges::any_of(
      known_.everything(), [&](const std::shared_ptr<Console>& held) {
        return held->generation() == Generation::ps4 &&
               held->is_paired_with(*as_) &&
               (held->hardware_id() == reached.hardware_id ||
                (reached.name && held->name() == reached.name->text()));
      });
}

auto PairedPlay::at_home() const -> Expected<HomeTarget, Refusal> {
  if (console_.is_forgotten()) return unexpected(Refusal::forgotten);
  auto keys = console_.core().keys_of(as_.account_id());
  if (!keys) return unexpected(Refusal::not_paired);
  auto address = console_.last_address();
  if (!address) return unexpected(Refusal::no_address);
  return HomeTarget{std::move(*address), console_.firmware(), std::move(*keys)};
}

auto PairedPlay::over_the_internet() const
    -> Expected<InternetTarget, Refusal> {
  if (console_.is_forgotten()) return unexpected(Refusal::forgotten);
  if (as_.is_off_psn()) return unexpected(Refusal::needs_psn_sign_in);
  const auto firmware = Firmware::newest(console_.generation());
  if (const auto known = console_.core().device_uid_of(as_.account_id()))
    return InternetTarget{*known, firmware};
  if (console_.generation() == Generation::ps4)
    return InternetTarget{DeviceUid::main_ps4(), firmware};
  return unexpected(Refusal::not_on_account);
}

auto PairedPlay::over_the_internet(std::span<const AccountConsole> on_account)
    const -> Expected<InternetTarget, Refusal> {
  auto target = over_the_internet();
  if ((!target && target.error() != Refusal::not_on_account) ||
      console_.generation() == Generation::ps4)
    return target;
  std::vector<AccountConsole> same_generation;
  for (const auto& listed : on_account)
    if (listed.generation() == console_.generation())
      same_generation.push_back(listed);
  if (target) {
    const auto identity_matches = [&](const AccountConsole& listed) {
      return listed.device_uid() == target.value().device_uid;
    };
    const auto count = std::ranges::count_if(same_generation, identity_matches);
    if (count > 1) return unexpected(Refusal::not_on_account);
    if (count == 1) {
      const auto found =
          std::ranges::find_if(same_generation, identity_matches);
      if (!found->remote_play_enabled())
        return unexpected(Refusal::remote_play_disabled);
      return InternetTarget{found->device_uid(),
                            Firmware::newest(console_.generation())};
    }
  }
  const std::array<AccountLink::Saved, 1> saved{
      AccountLink::Saved{console_.name(), std::nullopt}};
  const auto matches =
      AccountLink::matched(saved, AccountLink::listed(same_generation));
  if (matches.empty() ||
      (target && matches.front().rule != AccountLink::Rule::unique_name))
    return unexpected(Refusal::not_on_account);
  const auto& found = same_generation[matches.front().listed];
  if (!found.remote_play_enabled())
    return unexpected(Refusal::remote_play_disabled);
  return InternetTarget{found.device_uid(),
                        Firmware::newest(console_.generation())};
}

auto PairedPlay::found_on_account(KnownConsoles& known, const Console& console,
                                  users::PsUser& as,
                                  const platform::StopToken& stop)
    -> Expected<InternetTarget, Refusal> {
  const PairedPlay paired{console, as};
  const auto held = paired.over_the_internet();
  if ((!held && held.error() != Refusal::not_on_account) ||
      console.generation() == Generation::ps4)
    return held;
  const auto listed = known.consoles_on_account(as, console.generation(), stop);
  if (!listed) return unexpected(Refusal::account_unanswered);
  return paired.over_the_internet(listed.value());
}

}  // namespace libchiaking::consoles
