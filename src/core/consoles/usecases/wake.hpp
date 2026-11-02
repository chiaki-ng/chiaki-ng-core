/// @file
/// @brief Declares the Wake use case: waking a paired console from rest mode
/// over the home network, and optionally waiting until it is switched on.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "src/core/consoles/model/search/search_reply.hpp"

namespace libchiaking::platform {
class ClockProvider;
class NetworkProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles::usecases {

/// @brief Wakes consoles the user is paired with.
///
/// A wake-up datagram carries the credential derived from the pairing's
/// registration key, so only an account paired with the console can wake
/// it. Holds references only; may be used from several threads at once.
/// Everything it refers to must outlive it.
class Wake {
 public:
  /// @brief Makes the use case.
  ///
  /// @param network Sockets, DNS and interfaces of the calls that name no
  ///     provider of their own; must outlive this object.
  /// @param threading Makes the locks of the searches the waits run; must
  ///     outlive this object.
  /// @param clock The monotonic clock the waits are measured by; must outlive
  ///     this object.
  /// @param roster The registry's roster; must outlive this object.
  Wake(platform::NetworkProvider& network,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, Roster& roster) noexcept;
  /// @brief Not copyable.
  Wake(const Wake&) = delete;
  /// @brief Not copyable.
  auto operator=(const Wake&) -> Wake& = delete;

  /// @brief Sends one wake-up to the console's last address and returns,
  /// without checking that it woke.
  ///
  /// Blocks only while the address is resolved and the datagram sent.
  ///
  /// @param console The console.
  /// @param as The user whose pairing authorises the wake.
  /// @param over The provider to send through, held for the call; the
  ///     registry's own when it names none.
  /// @return Nothing once sent.
  /// @retval WakeRefusal::forgotten The console was forgotten.
  /// @retval WakeRefusal::not_paired The user holds no pairing, or its
  ///     registration key gives no wake credential.
  /// @retval WakeRefusal::no_address No address is known for the console.
  /// @retval WakeRefusal::network The datagram could not be sent.
  [[nodiscard]] auto now(Console& console, const users::PsUser& as,
                         HomeNetwork over) -> Expected<void, WakeRefusal>;
  /// @brief Finds the console, wakes it when it is in rest mode, and waits
  /// until it reports itself switched on.
  ///
  /// A private network search runs for the call, broadcasting and probing the
  /// console's last IPv4 address, or over a provider that cannot broadcast
  /// probing that address alone. A console already on returns at once; one
  /// in rest mode is sent a wake-up and watched until it answers as on. Each
  /// answer is recorded on the console. Blocks for up to
  /// Console::find_deadline to find it and then up to Console::ready_deadline
  /// for it to wake, noticing `stop` within about NetworkSearch::poll_step.
  ///
  /// @param console The console.
  /// @param as The user whose pairing authorises the wake.
  /// @param stop Cancels the wait.
  /// @param over The provider the search and the wake-up go through, held
  ///     for the call; the registry's own when it names none.
  /// @return Nothing once the console answered as switched on.
  /// @retval WakeRefusal::forgotten The console was forgotten.
  /// @retval WakeRefusal::not_paired The user holds no pairing, or its
  ///     registration key gives no wake credential.
  /// @retval WakeRefusal::not_found It did not answer within
  ///     Console::find_deadline; it is marked Presence::not_nearby.
  /// @retval WakeRefusal::still_in_rest_mode It was still in rest mode after
  ///     Console::ready_deadline.
  /// @retval WakeRefusal::network The search failed, the operating system
  ///     refused the probes, or the wake-up could not be sent.
  /// @retval WakeRefusal::no_address The console answered but no address is
  ///     held for it, or `over` cannot broadcast and no IPv4 address is held
  ///     to search at.
  /// @retval WakeRefusal::cancelled `stop` fired.
  [[nodiscard]] auto and_wait(Console& console, const users::PsUser& as,
                              const platform::StopToken& stop, HomeNetwork over)
      -> Expected<void, WakeRefusal>;

 private:
  /// @brief Sends a wake-up to an address with the credential from a pairing.
  ///
  /// @param network The provider to send through.
  /// @param address Where to send it.
  /// @param generation Which protocol version and port to use.
  /// @param keys The pairing's keys.
  /// @return Nothing once sent.
  /// @retval WakeRefusal::not_paired The registration key gives no wake
  ///     credential.
  /// @retval WakeRefusal::network The datagram could not be sent.
  [[nodiscard]] auto at(platform::NetworkProvider& network, const Host& address,
                        Generation generation, const PairingKeys& keys)
      -> Expected<void, WakeRefusal>;
  /// @brief Records a reply from the console and reads its power state.
  ///
  /// @param console The console.
  /// @param reply Its reply.
  /// @return Presence::rest_mode when it is in rest mode, Presence::ready
  ///     otherwise.
  auto sight(Console& console, const SearchReply& reply) -> Presence;

  /// @brief Sockets, DNS and interfaces.
  platform::NetworkProvider& network_;
  /// @brief Makes the locks of the searches the waits run.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Where answers are recorded.
  ConsoleFacts facts_;
};

}  // namespace libchiaking::consoles::usecases
