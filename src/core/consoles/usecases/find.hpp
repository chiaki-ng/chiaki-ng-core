/// @file
/// @brief Declares the Find use case: searching the home network for
/// consoles, and checking whether one known console is answering.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/network_search.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"
#include "src/core/consoles/adapters/search/shared_search.hpp"

#include <memory>
#include <vector>

namespace libchiaking::consoles::usecases {

/// @brief Finds consoles on the local network and records what is seen in
/// the roster.
///
/// Holds references only; may be used from several threads at once, the
/// shared search and the roster doing the locking. Everything it refers to
/// must outlive it.
class Find {
 public:
  /// @brief Makes the use case.
  ///
  /// @param network Sockets, DNS and interfaces; must outlive this object.
  /// @param threading Makes the sleepers of the beacons checks use; must
  ///     outlive this object.
  /// @param clock The monotonic clock; must outlive this object.
  /// @param search The registry's shared search; must outlive this object.
  /// @param roster The registry's roster; must outlive this object.
  Find(platform::NetworkProvider& network,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, SharedSearch& search,
       Roster& roster) noexcept;
  /// @brief Not copyable.
  Find(const Find&) = delete;
  /// @brief Not copyable.
  auto operator=(const Find&) -> Find& = delete;

  /// @brief Searches the home network until `stop` fires, telling `watcher`
  /// each new view.
  ///
  /// Joins the registry's shared search. Every console that answers is
  /// admitted to the roster and has the reply recorded; every other known
  /// console is marked out of sight, as Presence::unknown when nothing
  /// answers at all and Presence::not_nearby otherwise. Blocks the calling
  /// thread until `stop` fires or the search fails, noticing `stop` within
  /// about NetworkSearch::poll_step.
  ///
  /// @param ranges What to search.
  /// @param watcher Told each new view, on the calling thread.
  /// @param stop Ends the search.
  /// @return Nothing once `stop` fired.
  /// @retval SearchFailure::busy A search with other ranges is running.
  /// @retval SearchFailure::network The search could not start or failed.
  /// @retval SearchFailure::permission_denied The operating system refused
  ///     the probes.
  [[nodiscard]] auto on_my_network(const SearchRanges& ranges,
                                   NetworkWatcher& watcher,
                                   const platform::StopToken& stop)
      -> Expected<void, SearchFailure>;
  /// @brief Asks one console at its last address whether it is there.
  ///
  /// Sends a PS4 and a PS5 search to the address alone and waits up to
  /// Console::check_deadline for the console's reply, which is recorded on
  /// it. Without a reply the console is marked Presence::not_nearby. Blocks
  /// for up to Console::check_deadline, noticing `stop` within
  /// Console::check_wait_step.
  ///
  /// @param console The console.
  /// @param stop Cancels the check.
  /// @param over The provider the search goes through, held for the call;
  ///     the registry's own when it names none.
  /// @return Presence::ready or Presence::rest_mode, as the console answered.
  /// @retval Unreachable::forgotten The console was forgotten.
  /// @retval Unreachable::no_address No address is known for it.
  /// @retval Unreachable::network The search could not be sent or read.
  /// @retval Unreachable::cancelled `stop` fired.
  /// @retval Unreachable::timed_out No reply arrived in time.
  [[nodiscard]] auto check(Console& console, const platform::StopToken& stop,
                           HomeNetwork over) -> Expected<Presence, Unreachable>;

 private:
  /// @brief The beacon watcher check() uses to catch the one console's reply.
  class Lookout;

  /// @brief Records the search's replies in the roster and marks the consoles
  /// that did not answer.
  ///
  /// @param search The search.
  /// @return The consoles answering, in the order of the replies.
  auto nearby(const NetworkSearch& search)
      -> std::vector<std::shared_ptr<Console>>;

  /// @brief Sockets, DNS and interfaces.
  platform::NetworkProvider& network_;
  /// @brief Makes the sleepers of the beacons checks use.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief The registry's shared search.
  SharedSearch& search_;
  /// @brief The registry's roster.
  Roster& roster_;
  /// @brief Where sightings are recorded.
  ConsoleFacts facts_;
};

}  // namespace libchiaking::consoles::usecases
