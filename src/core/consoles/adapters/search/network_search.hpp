/// @file
/// @brief Declares NetworkSearch, a continuing search of the home network
/// for PS4 and PS5 consoles: it follows a SearchPlan, sends the probes
/// through a Beacon and keeps the set of consoles currently answering.
#pragma once

#include "libchiaking/detail/consoles/network_search.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/search/beacon.hpp"
#include "src/core/consoles/adapters/search/probe_diagnostics.hpp"
#include "src/core/consoles/model/search/search_plan.hpp"
#include "src/core/consoles/model/search/search_reply.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::consoles {

/// @brief One running search of the local IPv4 network.
///
/// After start(), each pump_once() re-reads the network interfaces every
/// interface_refresh_us, sends the probes the plan has due, reads up to
/// datagrams_per_pump replies and refreshes replies(), bumping revision()
/// when the set changed. Failed probes and long silences are logged through
/// ProbeDiagnostics. Over a network whose platform::NetworkCapabilities say
/// it cannot broadcast, the plan leaves broadcast addresses out and only the
/// pinned addresses, the consoles that replied and the swept subnets are
/// probed. A subnet of the ranges with a network provider of its own gets a
/// beacon over that provider, and every probe to an address the narrowest
/// such subnet holds goes through it.
///
/// One thread drives the search (start, pump_once, poll, wait, stop,
/// keep_probing and the getters). wake() and cancel() may be called from any
/// thread; once cancelled, a search stays cancelled. The providers must
/// outlive it.
class NetworkSearch final : private Beacon::Watcher {
 public:
  /// @brief Why a search call failed.
  enum class Failure : std::uint8_t {
    /// @brief The beacon settings are unusable, the bind address is not
    /// IPv4, or there are too many interfaces.
    invalid_argument,
    /// @brief The search is not started, already started, or cancelled.
    invalid_state,
    /// @brief The socket, the interface list or a receive failed.
    network,
  };

  /// @brief How long poll() waits by default.
  static constexpr std::chrono::milliseconds poll_step{100};
  /// @brief The longest wait poll() allows.
  static constexpr std::chrono::milliseconds maximum_poll{200};
  /// @brief The longest single wait on the socket.
  static constexpr std::chrono::milliseconds longest_wait{100};
  /// @brief Most datagrams read in one pump_once().
  static constexpr std::size_t datagrams_per_pump = 128;
  /// @brief Time between re-reads of the network interfaces, in microseconds.
  static constexpr std::uint64_t interface_refresh_us = 2'000'000;

  /// @brief Makes a search that has not started.
  ///
  /// @param network Sockets, DNS and interfaces of the broadcasts and of
  ///     every subnet without a provider of its own; must outlive this
  ///     object.
  /// @param threading Makes the lock that guards the beacons; must outlive
  ///     this object.
  /// @param clock The monotonic clock the plan runs on; must outlive this
  ///     object.
  /// @param ranges What to search; it holds the providers of its subnets for
  ///     as long as this object lives.
  /// @param log Where probe diagnostics go; null logs nothing. Must outlive
  ///     this object.
  /// @param beacon Where to bind and which ports to search; its targets are
  ///     ignored, the plan choosing them.
  NetworkSearch(platform::NetworkProvider& network,
                platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock, SearchRanges ranges,
                platform::DiagnosticsProvider* log = nullptr,
                Beacon::Settings beacon = {});
  /// @brief Closes the beacon, if open.
  ~NetworkSearch() override;

  /// @brief Not copyable: it owns a socket.
  NetworkSearch(const NetworkSearch&) = delete;
  /// @brief Not copyable.
  auto operator=(const NetworkSearch&) -> NetworkSearch& = delete;

  /// @brief Binds the beacons and starts a fresh plan, keeping the pinned
  /// addresses.
  ///
  /// With unicast ranges, one beacon is bound over each distinct provider the
  /// subnets name, besides the one over the search's own network.
  ///
  /// @return Nothing once started.
  /// @retval Failure::invalid_state Already started, or cancelled.
  /// @retval Failure::invalid_argument The bind address is not IPv4, or a
  ///     beacon refused its settings.
  /// @retval Failure::network A beacon could not bind; none is left open.
  [[nodiscard]] auto start() -> Expected<void, Failure>;
  /// @brief Does the search's due work once, without waiting.
  ///
  /// Blocks only for the interface list and the sends.
  ///
  /// @return Nothing on success.
  /// @retval Failure::invalid_state Not started, or cancelled meanwhile.
  /// @retval Failure::network The interface list or a receive failed.
  /// @retval Failure::invalid_argument There are too many interfaces.
  [[nodiscard]] auto pump_once() -> Expected<void, Failure>;
  /// @brief Pumps, waits for replies or the next due work, and pumps again.
  ///
  /// Blocks for at most `timeout`, clamped to maximum_poll, and returns early
  /// when woken.
  ///
  /// @param timeout The longest wait.
  /// @return Nothing on success.
  /// @retval Failure Any failure of pump_once(); Failure::invalid_state when
  ///     cancelled.
  [[nodiscard]] auto poll(std::chrono::milliseconds timeout = poll_step)
      -> Expected<void, Failure>;
  /// @brief Closes the beacons and drops the plan; does nothing when not
  /// started.
  ///
  /// @return Nothing on success.
  /// @retval Failure::network A socket did not close cleanly; every beacon is
  ///     dropped regardless.
  [[nodiscard]] auto stop() -> Expected<void, Failure>;
  /// @brief Adds an address to every broadcast round, now and after any later
  /// start().
  ///
  /// @param address The address; IPv6 is ignored.
  void keep_probing(platform::IpAddress address);

  /// @brief What the search covers.
  ///
  /// @return The ranges given at construction.
  auto ranges() const noexcept -> const SearchRanges& { return ranges_; }
  /// @brief The consoles answering, one reply each, sorted by hardware id.
  ///
  /// @return The replies as of the last pump; valid until the next pump.
  auto replies() const noexcept -> std::span<const SearchReply> {
    return replies_;
  }
  /// @brief A counter bumped each time replies() changes.
  ///
  /// @return The revision; zero until the first change.
  auto revision() const noexcept -> std::uint64_t { return revision_; }
  /// @brief How far the search has got.
  ///
  /// @return The plan's progress with Progress::permission_denied set when
  ///     the last probes were refused; nothing when not started.
  auto progress() const -> std::optional<SearchPlan::Progress>;
  /// @brief When the search next has work to do.
  ///
  /// @return The monotonic time in microseconds, or zero when not started.
  auto next_deadline_microseconds() const -> std::uint64_t;
  /// @brief The address and port the beacon over the search's own network is
  /// bound to.
  ///
  /// @return The endpoint.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::network The socket could not say.
  [[nodiscard]] auto local_endpoint() const
      -> Expected<platform::SocketEndpoint, Failure>;
  /// @brief Whether cancel() was called.
  ///
  /// @return True once cancelled.
  auto cancelled() const noexcept -> bool {
    return cancelled_.load(std::memory_order_acquire);
  }

  /// @brief Blocks until a reply is waiting, the next due work, wake() or
  /// `timeout`, and never longer than longest_wait. Returns at once when
  /// cancelled.
  ///
  /// With beacons over several providers, whose sockets cannot share one
  /// poller, the wait is split evenly between them, so a reply on one may be
  /// read up to that share late.
  ///
  /// @param timeout The longest wait.
  void wait(std::chrono::milliseconds timeout);
  /// @brief Ends a wait() in progress early. Safe from any thread.
  void wake() noexcept;
  /// @brief Cancels the search and ends any wait. Safe from any thread; the
  /// driving thread sees Failure::invalid_state from its next call.
  void cancel() noexcept;

 private:
  /// @brief The search failure a beacon failure is reported as.
  ///
  /// @param failure The beacon failure.
  /// @return The matching failure; a short send is Failure::network.
  static constexpr auto failure_of(Beacon::Failure failure) noexcept
      -> Failure {
    return failure == Beacon::Failure::invalid_argument
               ? Failure::invalid_argument
           : failure == Beacon::Failure::invalid_state ? Failure::invalid_state
                                                       : Failure::network;
  }

  /// @brief Hands a reply to the plan, unless cancelled.
  ///
  /// @param reply The reply.
  void on_reply(SearchReply reply) override;
  /// @brief Counts a probe in the diagnostics.
  ///
  /// @param destination Where it went.
  /// @param failure Why it failed, or nothing.
  void on_probe(platform::SocketEndpoint destination,
                std::optional<Beacon::ProbeFailure> failure) override;
  /// @brief Counts a received datagram in the diagnostics.
  ///
  /// @param admitted Whether it was a console's reply.
  void on_datagram(bool admitted) override;

  /// @brief Re-reads the interfaces into the plan, starts the diagnostics the
  /// first time, and schedules the next re-read.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing on success.
  /// @retval Failure::network The interfaces could not be listed.
  /// @retval Failure::invalid_argument There are too many interfaces.
  [[nodiscard]] auto refresh_interfaces(std::uint64_t now_us)
      -> Expected<void, Failure>;
  /// @brief Sends a search to every destination the plan has due, stopping at
  /// the first send the operating system refuses.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing on success; other send failures are not reported here.
  /// @retval Failure::invalid_state Cancelled meanwhile.
  [[nodiscard]] auto probe(std::uint64_t now_us) -> Expected<void, Failure>;
  /// @brief Sends a search to one address through the beacon that reaches
  /// it.
  ///
  /// @param destination The address.
  /// @return Nothing when sent.
  /// @retval Beacon::ProbeFailure Why it was not.
  [[nodiscard]] auto send_search_to(platform::IpAddress destination)
      -> Expected<void, Beacon::ProbeFailure>;
  /// @brief Returns the beacon an address is probed through.
  ///
  /// @param destination The address.
  /// @return The beacon over the provider of the narrowest subnet that holds
  ///     @p destination, or the one over the search's own network when that
  ///     subnet names none or no subnet holds it.
  auto beacon_for(platform::IpAddress destination) -> Beacon&;
  /// @brief Reads up to datagrams_per_pump datagrams from one beacon.
  ///
  /// @param beacon The beacon.
  /// @return Nothing on success.
  /// @retval Failure::invalid_state Cancelled meanwhile.
  /// @retval Failure Why a receive failed.
  [[nodiscard]] auto receive(Beacon& beacon) -> Expected<void, Failure>;
  /// @brief The earlier of the next interface re-read and the plan's next
  /// work.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return The monotonic time, in microseconds.
  auto next_deadline_us(std::uint64_t now_us) const -> std::uint64_t;

  /// @brief Sockets, DNS and interfaces.
  platform::NetworkProvider& network_;
  /// @brief Makes the sleepers of the beacons the search opens.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief What the search covers.
  SearchRanges ranges_;
  /// @brief Where the beacon binds and which ports it searches.
  Beacon::Settings settings_;
  /// @brief The probe log.
  ProbeDiagnostics probes_;
  /// @brief Addresses added with keep_probing().
  std::vector<platform::IpAddress> pinned_;
  /// @brief The schedule; present while started.
  std::optional<SearchPlan> plan_;
  /// @brief A beacon over a provider one of the subnets names.
  struct Route {
    /// @brief The provider, held by #ranges_.
    platform::NetworkProvider* network{};
    /// @brief The beacon over it.
    std::unique_ptr<Beacon> beacon;
  };

  /// @brief The discovery socket over the search's own network; null when
  /// not started.
  std::unique_ptr<Beacon> beacon_;
  /// @brief The beacons over the subnets' own providers, one per provider;
  /// empty when not started.
  std::vector<Route> routes_;
  /// @brief Guards replacing #beacon_ and #routes_ against wake() from
  /// another thread.
  platform::Mutex beacon_mutex_;
  /// @brief Whether cancel() was called.
  std::atomic<bool> cancelled_{false};
  /// @brief The consoles answering as of the last pump.
  std::vector<SearchReply> replies_;
  /// @brief Bumped each time #replies_ changes.
  std::uint64_t revision_{};
  /// @brief When the interfaces are next re-read, in monotonic microseconds.
  std::uint64_t next_interface_us_{};
  /// @brief Whether the operating system refused the last round of probes.
  bool permission_denied_{};
  /// @brief Waits when no beacon is open, and can be woken.
  net::http::SleepWaiter idle_;
};

}  // namespace libchiaking::consoles
