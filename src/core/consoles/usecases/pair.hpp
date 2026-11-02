/// @file
/// @brief Declares the Pair use case: pairing the user with a console, at
/// home by the PIN it shows or over the internet from the account's console
/// list, and saving the pairing.
#pragma once

#include "libchiaking/detail/consoles/account_console.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/internet_pairing.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"

#include <memory>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class CryptoProvider;
class DiagnosticsProvider;
class HttpProvider;
class NetworkProvider;
class SecureRandomProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles::usecases {

/// @brief Pairs users with consoles and keeps the pairings in the roster.
///
/// Holds references only; may be used from several threads at once, each
/// attempt running on its caller's thread. Everything it refers to must
/// outlive it.
class Pair {
 public:
  /// @brief A paired and saved console, or why pairing failed.
  using Paired = Expected<std::shared_ptr<Console>, PairingFailure>;

  /// @brief Makes the use case.
  ///
  /// @param network Sockets and DNS for pairing at home; must outlive this
  ///     object.
  /// @param http HTTP for the account's console list; must outlive this
  ///     object.
  /// @param crypto Ciphers for the pairing request; must outlive this object.
  /// @param random Fresh keys for the pairing request; must outlive this
  ///     object.
  /// @param threading Makes the sleepers pairing at home waits on; must
  ///     outlive this object.
  /// @param clock The monotonic clock; must outlive this object.
  /// @param internet How a console is paired over the internet; null when
  ///     the host offers none.
  /// @param roster The registry's roster; must outlive this object.
  /// @param log Where pairing logs; null logs nothing.
  Pair(platform::NetworkProvider& network, platform::HttpProvider& http,
       platform::CryptoProvider& crypto, platform::SecureRandomProvider& random,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, InternetPairing* internet,
       Roster& roster, platform::DiagnosticsProvider* log) noexcept;
  /// @brief Not copyable.
  Pair(const Pair&) = delete;
  /// @brief Not copyable.
  auto operator=(const Pair&) -> Pair& = delete;

  /// @brief Pairs with a console a search found, at its last address and for
  /// its firmware.
  ///
  /// Blocks for up to HomePairing::deadline; see HomePairing::run. Progress,
  /// ending with PairingStep::saving, goes to `watcher`.
  ///
  /// @param as The user to pair.
  /// @param found The console.
  /// @param pin The PIN the console shows.
  /// @param stop Cancels the attempt.
  /// @param watcher Told of progress; may be null.
  /// @return The console, now paired and saved.
  /// @retval PairingFailure::Reason::unreachable No address is known for the
  ///     console, or it could not be reached.
  /// @retval PairingFailure::Reason::not_saved The console accepted but the
  ///     store refused the pairing.
  /// @retval PairingFailure Any other failure of the attempt.
  [[nodiscard]] auto at_home(const users::PsUser& as, Console& found,
                             PairingPin pin, const platform::StopToken& stop,
                             PairingWatcher* watcher) -> Paired;
  /// @brief Pairs with a console at an address the user typed, assuming the
  /// newest firmware of `generation` until the console's reply says
  /// otherwise.
  ///
  /// Blocks for up to HomePairing::deadline; see HomePairing::run.
  ///
  /// @param as The user to pair.
  /// @param at The console's address.
  /// @param generation Whether a PS4 or a PS5 is expected there.
  /// @param pin The PIN the console shows.
  /// @param stop Cancels the attempt.
  /// @param watcher Told of progress; may be null.
  /// @return The console, now paired and saved.
  /// @retval PairingFailure::Reason::address_not_for_generation The address
  ///     is an IPv6 literal and a PS4 is expected.
  /// @retval PairingFailure::Reason::not_saved The store refused the pairing.
  /// @retval PairingFailure Any other failure of the attempt.
  [[nodiscard]] auto at_home(const users::PsUser& as, const Host& at,
                             Generation generation, PairingPin pin,
                             const platform::StopToken& stop,
                             PairingWatcher* watcher) -> Paired;
  /// @brief Lists the consoles on the user's account, linking each paired PS5
  /// to the one PSN lists for it.
  ///
  /// See AccountConsoles::of and AccountLink::linked; blocks for the PSN
  /// request and the store writes of new links.
  ///
  /// @param of The user.
  /// @param generation Which consoles to list.
  /// @param stop Cancels the request.
  /// @return The consoles, in PSN's order.
  /// @retval users::Unanswered Any failure of the console list.
  [[nodiscard]] auto on_account(users::PsUser& of, Generation generation,
                                const platform::StopToken& stop)
      -> Expected<std::vector<AccountConsole>, users::Unanswered>;
  /// @brief Pairs with a console on the user's account through the host's
  /// InternetPairing, then saves the pairing linked to the console's device
  /// uid and named as PSN names it.
  ///
  /// Blocks for as long as InternetPairing::pair takes, then for the store
  /// writes.
  ///
  /// @param as The user to pair.
  /// @param which The console, as on_account() listed it.
  /// @param watcher Told of progress; may be null.
  /// @param stop Cancels the attempt.
  /// @return The console, now paired and saved.
  /// @retval PairingFailure::Reason::internet_pairing_missing The host offers
  ///     no internet pairing.
  /// @retval PairingFailure::Reason::unanswered The user is off PSN.
  /// @retval PairingFailure::Reason::remote_play_disabled PSN lists Remote
  ///     Play as off on the console.
  /// @retval PairingFailure::Reason::not_saved The store refused the pairing.
  /// @retval PairingFailure Any failure InternetPairing::pair reports.
  [[nodiscard]] auto over_the_internet(users::PsUser& as,
                                       const AccountConsole& which,
                                       PairingWatcher* watcher,
                                       const platform::StopToken& stop)
      -> Paired;

 private:
  /// @brief Pairs at home with a console at an address, for a firmware, and
  /// saves the pairing.
  ///
  /// @param as The user to pair.
  /// @param at The console's address.
  /// @param firmware The firmware the request is made for.
  /// @param pin The PIN the console shows.
  /// @param stop Cancels the attempt.
  /// @param watcher Told of progress; may be null.
  /// @return The console, now paired and saved.
  /// @retval PairingFailure::Reason::address_not_for_generation The address
  ///     cannot reach the firmware's generation.
  /// @retval PairingFailure::Reason::needs_online_id A PS4 below 7.0 needs an
  ///     online id the user lacks.
  /// @retval PairingFailure::Reason::not_saved The store refused the pairing.
  /// @retval PairingFailure Any failure of HomePairing::run.
  [[nodiscard]] auto at_address(const users::PsUser& as, const Host& at,
                                Firmware firmware, PairingPin pin,
                                const platform::StopToken& stop,
                                PairingWatcher* watcher) -> Paired;

  /// @brief Sockets and DNS for pairing at home.
  platform::NetworkProvider& network_;
  /// @brief HTTP for the account's console list.
  platform::HttpProvider& http_;
  /// @brief Ciphers for the pairing request.
  platform::CryptoProvider& crypto_;
  /// @brief Fresh keys for the pairing request.
  platform::SecureRandomProvider& random_;
  /// @brief Makes the sleepers pairing at home waits on.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief How a console is paired over the internet; null when the host
  /// offers none.
  InternetPairing* internet_;
  /// @brief Where pairings, links and names are recorded.
  ConsoleFacts facts_;
  /// @brief Where pairing logs; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::consoles::usecases
