/// @file
/// @brief Declares SharedSearch, which lets several callers of
/// KnownConsoles::find_on_my_network share one NetworkSearch, so the home
/// network is searched once however many screens are looking.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/network_search.hpp"
#include "libchiaking/detail/expected.hpp"
#include "src/core/consoles/adapters/search/network_search.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace libchiaking::consoles {

/// @brief The one network search a registry runs, shared by every caller
/// that joins it.
///
/// The first caller to join starts a search with its ranges; later callers
/// with the same ranges join that search, and callers with other ranges are
/// refused while it runs. Each caller holds a Seat. Seats take turns: one
/// seat steps the search while the others wait for the step, and every seat
/// is handed each new NetworkView once. The search stops when the last seat
/// leaves.
///
/// Safe to use from several threads at once. The providers must outlive it.
class SharedSearch {
 public:
  /// @brief Turns the search's current replies into the consoles nearby,
  /// recording what they show; called by the seat that steps.
  using Nearby = std::function<std::vector<std::shared_ptr<Console>>(
      const NetworkSearch&)>;

  /// @brief What one seat's turn produced.
  struct Turn {
    /// @brief The outcome of the step this seat took; success when it
    /// waited for another seat's step instead.
    Expected<void, SearchFailure> stepped{};
    /// @brief A view this seat has not been handed yet; empty when nothing
    /// changed.
    std::optional<NetworkView> fresh{};
  };

  /// @brief One caller's place in the shared search; leaving is done by its
  /// destructor.
  ///
  /// Used by one thread. Must not outlive the SharedSearch.
  class Seat {
   public:
    /// @brief Takes a seat in a search already joined.
    ///
    /// @param shared The shared search; must outlive the seat.
    /// @param search The search join() returned.
    Seat(SharedSearch& shared, std::shared_ptr<NetworkSearch> search) noexcept;
    /// @brief Not copyable: each seat leaves once.
    Seat(const Seat&) = delete;
    /// @brief Not copyable.
    auto operator=(const Seat&) -> Seat& = delete;
    /// @brief Leaves the search, stopping it when this was the last seat.
    ~Seat();

    /// @brief Steps the search, or waits for the seat that is stepping it.
    ///
    /// A step polls the search for up to NetworkSearch::poll_step and, when
    /// the replies or the progress changed, builds a new view with
    /// `nearby`. A seat that finds another stepping waits up to the same
    /// time for it to finish.
    ///
    /// @param nearby Turns replies into consoles, if this seat steps.
    /// @return The outcome of this seat's step, if it took one, and any
    ///     view it has not yet been handed.
    [[nodiscard]] auto turn(const Nearby& nearby) -> Turn;

   private:
    /// @brief The shared search.
    SharedSearch& shared_;
    /// @brief The search this seat joined.
    std::shared_ptr<NetworkSearch> search_;
    /// @brief The revision of the last view handed to this seat.
    std::uint64_t delivered_{};
  };

  /// @brief Makes a shared search with none running.
  ///
  /// @param network Sockets, DNS and interfaces; must outlive this object.
  /// @param threading Makes the locks and the condition variable seats wait
  ///     on, and those of every search started; must outlive this object.
  /// @param clock The monotonic clock; must outlive this object.
  /// @param log Where search diagnostics go; null logs nothing. Must outlive
  ///     this object.
  SharedSearch(platform::NetworkProvider& network,
               platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock,
               platform::DiagnosticsProvider* log);
  /// @brief Not copyable.
  SharedSearch(const SharedSearch&) = delete;
  /// @brief Not copyable.
  auto operator=(const SharedSearch&) -> SharedSearch& = delete;

  /// @brief Joins the running search, or starts one with `ranges`.
  ///
  /// The caller must then take a Seat with the returned search.
  ///
  /// @param ranges What to search.
  /// @return The search.
  /// @retval SearchFailure::busy A search with other ranges is running.
  /// @retval SearchFailure::network A new search could not be started.
  [[nodiscard]] auto join(const SearchRanges& ranges)
      -> Expected<std::shared_ptr<NetworkSearch>, SearchFailure>;

 private:
  /// @brief Gives up a seat; the last one out stops the search.
  void leave();
  /// @brief Polls the search once and refreshes the view when anything
  /// changed.
  ///
  /// @param search The search.
  /// @param nearby Turns replies into consoles.
  /// @return Nothing on success.
  /// @retval SearchFailure::network The poll failed or the search is stopped.
  /// @retval SearchFailure::permission_denied The operating system refused
  ///     the probes; the view says so too.
  [[nodiscard]] auto step(NetworkSearch& search, const Nearby& nearby)
      -> Expected<void, SearchFailure>;
  /// @brief Builds a new view from the search's progress and the consoles
  /// nearby, and publishes it to every seat.
  ///
  /// @param search The search.
  /// @param nearby Turns replies into consoles.
  void look_again(const NetworkSearch& search, const Nearby& nearby);

  /// @brief Sockets, DNS and interfaces.
  platform::NetworkProvider& network_;
  /// @brief Makes the locks of every search started.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Where search diagnostics go; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
  /// @brief Guards #search_ and #searchers_.
  platform::Mutex mutex_;
  /// @brief Guards #stepping_ and the published view; held while the search
  /// is stopped.
  platform::Mutex turn_;
  /// @brief Signalled when a step finishes.
  platform::ConditionVariable stepped_;
  /// @brief Whether a seat is stepping the search now.
  bool stepping_{};
  /// @brief The running search; null when none runs.
  std::shared_ptr<NetworkSearch> search_;
  /// @brief How many seats hold the running search.
  std::size_t searchers_{};
  /// @brief The latest view.
  NetworkView view_;
  /// @brief Bumped each time #view_ is replaced; zero before the first view.
  std::uint64_t view_revision_{};
  /// @brief The search's reply revision the latest view was built from.
  std::uint64_t seen_revision_{};
};

}  // namespace libchiaking::consoles
