#include "src/core/consoles/usecases/pair.hpp"

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/consoles/adapters/pairing/account_consoles.hpp"
#include "src/core/consoles/adapters/pairing/account_link.hpp"
#include "src/core/consoles/adapters/pairing/home_pairing.hpp"

#include <optional>
#include <utility>

namespace libchiaking::consoles::usecases {

Pair::Pair(platform::NetworkProvider& network, platform::HttpProvider& http,
           platform::CryptoProvider& crypto,
           platform::SecureRandomProvider& random,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock, InternetPairing* internet,
           Roster& roster, platform::DiagnosticsProvider* log) noexcept
    : network_(network),
      http_(http),
      crypto_(crypto),
      random_(random),
      threading_(threading),
      clock_(clock),
      internet_(internet),
      facts_(roster),
      log_(log) {}

auto Pair::at_home(const users::PsUser& as, Console& found, PairingPin pin,
                   const platform::StopToken& stop, PairingWatcher* watcher)
    -> Paired {
  const auto at = found.last_address();
  if (!at)
    return unexpected(PairingFailure{PairingFailure::Reason::unreachable});
  return at_address(as, *at, found.firmware(), std::move(pin), stop, watcher);
}

auto Pair::at_home(const users::PsUser& as, const Host& at,
                   Generation generation, PairingPin pin,
                   const platform::StopToken& stop, PairingWatcher* watcher)
    -> Paired {
  return at_address(as, at, Firmware::newest(generation), std::move(pin), stop,
                    watcher);
}

auto Pair::at_address(const users::PsUser& as, const Host& at,
                      Firmware firmware, PairingPin pin,
                      const platform::StopToken& stop, PairingWatcher* watcher)
    -> Paired {
  if (!at.reaches(firmware.generation()))
    return unexpected(
        PairingFailure{PairingFailure::Reason::address_not_for_generation});
  auto request = HomePairing::Request::of(as, firmware, std::move(pin));
  if (!request) return unexpected(request.error());
  HomePairing pairing{network_, threading_, crypto_, clock_, random_, log_};
  const auto began_us = clock_.now_microseconds();
  auto paired = pairing.run(std::move(request.value()), at.text(), stop,
                            rp_session_port, HomePairing::deadline, watcher);
  if (!paired) return unexpected(paired.error());
  if (watcher != nullptr)
    watcher->on_progress({.step = PairingStep::saving,
                          .elapsed = HomePairing::elapsed_since(
                              began_us, clock_.now_microseconds())});
  auto kept = facts_.paired(as, std::move(paired.value()), at);
  if (!kept)
    return unexpected(PairingFailure{PairingFailure::Reason::not_saved});
  return std::move(kept.value());
}

auto Pair::on_account(users::PsUser& of, Generation generation,
                      const platform::StopToken& stop)
    -> Expected<std::vector<AccountConsole>, users::Unanswered> {
  auto listed = AccountConsoles{http_, clock_, log_}.of(of, generation, stop);
  if (!listed) return unexpected(listed.error());
  if (generation == Generation::ps5)
    AccountLink{facts_.roster(), log_}.linked(
        of, AccountLink::listed(listed.value().consoles));
  return std::move(listed.value().consoles);
}

auto Pair::over_the_internet(users::PsUser& as, const AccountConsole& which,
                             PairingWatcher* watcher,
                             const platform::StopToken& stop) -> Paired {
  using Reason = PairingFailure::Reason;
  if (internet_ == nullptr)
    return unexpected(PairingFailure{Reason::internet_pairing_missing});
  if (as.is_off_psn())
    return unexpected(PairingFailure{
        .reason = Reason::unanswered,
        .unanswered =
            users::Unanswered{users::Unanswered::Reason::signed_out}});
  if (!which.remote_play_enabled())
    return unexpected(PairingFailure{Reason::remote_play_disabled});
  const auto began_us = clock_.now_microseconds();
  auto paired = internet_->pair(as, which.device_uid(), which.generation(),
                                watcher, stop);
  if (!paired) return unexpected(paired.error());
  if (watcher != nullptr)
    watcher->on_progress({.step = PairingStep::saving,
                          .elapsed = HomePairing::elapsed_since(
                              began_us, clock_.now_microseconds())});
  auto kept = facts_.paired(as, std::move(paired.value()), std::nullopt,
                            which.device_uid());
  if (!kept) return unexpected(PairingFailure{Reason::not_saved});
  if (!which.is_main_ps4())
    if (const auto named =
            ConsoleName::create(which.name(), ConsoleName::Source::account))
      facts_.named(*kept.value(), *named);
  return std::move(kept.value());
}

}  // namespace libchiaking::consoles::usecases
