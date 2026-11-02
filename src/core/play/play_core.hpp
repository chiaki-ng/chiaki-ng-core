/// @file
/// @brief Declares Play::Core, the private half of Play that wires the start
/// use case, the live-session slot and the known datacenters together.
#pragma once

#include "libchiaking/detail/play/play.hpp"
#include "libchiaking/platform/runtime/utc_offset.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/play/adapters/nat/stun_probe.hpp"
#include "src/core/play/adapters/session/live_session.hpp"
#include "src/core/play/usecases/nat_type_check.hpp"
#include "src/core/play/usecases/start.hpp"

#include <optional>

namespace libchiaking::play {

/// @brief The implementation behind the public Play facade.
///
/// Owns the one LiveSession slot, the Datacenters view of the datacenter
/// store and the usecases::Start that fills the slot. While it lives it tells
/// the InternetPairer whether a session is playing, so a pairing is refused
/// during play; the destructor withdraws that watch. Every collaborator passed
/// in must outlive it. Not copyable; thread rules are those of its parts.
class Play::Core {
 public:
  /// @brief Builds the core, whose starts clear titles with the catalog, wait
  /// on the platform's clock and keep the system's time zone, and registers
  /// the live-session watch with the pairer.
  ///
  /// @param platform The host's providers; must outlive the core.
  /// @param consoles The consoles a start may play on; must outlive the core.
  /// @param catalog Clears cloud titles before launch; must outlive the core.
  /// @param datacenters Where measured cloud datacenters are kept; must
  ///     outlive the core.
  /// @param pairer The internet pairer; must outlive the core.
  /// @param log Receives log lines; null falls back to the platform's
  ///     diagnostics provider.
  Core(platform::PlatformFactory& platform, consoles::KnownConsoles& consoles,
       catalog::Catalog& catalog, DatacenterStore& datacenters,
       InternetPairer& pairer, platform::DiagnosticsProvider* log);
  /// @brief Builds the core with the collaborators every start uses, and
  /// registers the live-session watch with the pairer.
  ///
  /// @param platform The host's providers; must outlive the core.
  /// @param consoles The consoles a start may play on; must outlive the core.
  /// @param catalog Names cloud services in launch requests; must outlive the
  ///     core.
  /// @param datacenters Where measured cloud datacenters are kept; must
  ///     outlive the core.
  /// @param pairer The internet pairer; must outlive the core.
  /// @param log Receives log lines; null falls back to the platform's
  ///     diagnostics provider.
  /// @param collaborators What every start clears titles, waits and keeps
  ///     time with; each reference must outlive the core.
  Core(platform::PlatformFactory& platform, consoles::KnownConsoles& consoles,
       catalog::Catalog& catalog, DatacenterStore& datacenters,
       InternetPairer& pairer, platform::DiagnosticsProvider* log,
       StartCollaborators collaborators);
  /// @brief Not copyable; the core owns the live-session slot.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; the core owns the live-session slot.
  auto operator=(const Core&) -> Core& = delete;
  /// @brief Withdraws the pairer's live-session watch.
  ~Core();

  /// @brief Gives the start use case.
  ///
  /// @return The use case, owned by the core.
  auto start() noexcept -> usecases::Start& { return start_; }
  /// @brief Gives the live-session slot.
  ///
  /// @return The slot, owned by the core.
  auto live() noexcept -> LiveSession& { return live_; }
  /// @brief Gives the view of the known datacenters.
  ///
  /// @return The view, owned by the core.
  auto datacenters() noexcept -> Datacenters& { return datacenters_; }
  /// @brief Gives the NAT type check.
  ///
  /// @return The use case, owned by the core.
  auto nat_type() noexcept -> usecases::NatTypeCheck& { return nat_type_; }
  /// @brief Gives the diagnostics sink.
  ///
  /// @return The sink; may be null.
  auto log() noexcept -> platform::DiagnosticsProvider* { return log_; }

 private:
  /// @brief Builds the core with the given collaborators, or with its own.
  ///
  /// @param platform The host's providers.
  /// @param consoles The consoles a start may play on.
  /// @param catalog The catalog.
  /// @param datacenters Where measured cloud datacenters are kept.
  /// @param pairer The internet pairer.
  /// @param log Receives log lines; null falls back to the platform's.
  /// @param given The collaborators to start with; empty for the core's own.
  Core(platform::PlatformFactory& platform, consoles::KnownConsoles& consoles,
       catalog::Catalog& catalog, DatacenterStore& datacenters,
       InternetPairer& pairer, platform::DiagnosticsProvider* log,
       std::optional<StartCollaborators> given);
  /// @brief Gives the collaborators the core owns.
  ///
  /// @return The catalog's clearance, the core's waiter and the system time
  ///     zone, with the session's own settings.
  auto own_collaborators() -> StartCollaborators;

  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The pairer told whether a session is live; outlives the core.
  InternetPairer& pairer_;
  /// @brief Reads and forgets the measured datacenters per service.
  Datacenters datacenters_;
  /// @brief The slot holding the one live session.
  LiveSession live_;
  /// @brief Reads how STUN servers see the host.
  StunProbe stun_;
  /// @brief Finds the host's NAT type through stun_.
  usecases::NatTypeCheck nat_type_;
  /// @brief Clears cloud titles with the catalog unless other collaborators
  /// were given.
  CatalogClearance catalog_clearance_;
  /// @brief Waits during cloud launches unless other collaborators were
  /// given.
  net::http::SleepWaiter launch_waiter_;
  /// @brief The system time zone, kept unless other collaborators were given.
  platform::SystemUtcOffset time_zone_;
  /// @brief Starts sessions into the live slot; declared last so it is built
  /// after the parts it refers to.
  usecases::Start start_;
};

}  // namespace libchiaking::play
