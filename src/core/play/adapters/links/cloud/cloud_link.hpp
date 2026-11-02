/// @file
/// @brief Declares CloudProvisioning, which builds and runs the cloud launch
/// a cloud link starts with, and CloudLaunchSeams, the collaborators tests
/// may replace.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/links/link.hpp"

#include "libchiaking/platform/factory.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/cloud/server_slot.hpp"
#include "src/core/play/adapters/cloud_launch/title_clearance.hpp"
#include "src/core/psn/server_clock.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"

#include <memory>
#include <optional>

namespace libchiaking::diagnostics {
class Trace;
}

namespace libchiaking::play {

/// @brief Collaborators of a cloud launch a test may replace; each null
/// member means the real one is used. Plain data.
struct CloudLaunchSeams {
  /// @brief Clears the title instead of the catalog.
  play::TitleClearance* clearance{};
  /// @brief Waits during the launch instead of a socket sleep.
  net::http::PollWaiter* waiter{};
  /// @brief Supplies PS's server time.
  psn::ServerClock* server_clock{};
  /// @brief Reads and records datacenters instead of a view over the
  /// configuration's store.
  play::Datacenters* datacenters{};
  /// @brief Supplies the time zone offset instead of the system's.
  const platform::UtcOffsetProvider* utc_offset{};
};

/// @brief The launch half of a cloud link: owns the CloudLaunch::Core that
/// reserves a server, with the defaults it falls back to when no seam is
/// given.
///
/// A configuration without a PSN account, a clearance or a product cannot
/// launch; that is remembered and reported as the unmet precondition. Owned
/// by the cloud link and run on its thread. Not copyable.
class CloudProvisioning {
 public:
  /// @brief The cloud launch.
  using Core = play::CloudLaunch::Core;

  /// @brief The providers the launch talks through, all borrowed.
  struct Providers {
    /// @brief Sends requests to PS.
    platform::HttpProvider& http;
    /// @brief Opens the measurement sockets.
    platform::NetworkProvider& network;
    /// @brief The monotonic clock.
    platform::ClockProvider& clock;
    /// @brief Makes the launch's locks.
    platform::ThreadingProvider& threading;
    /// @brief Randomness for the probes.
    platform::SecureRandomProvider& random;
    /// @brief Records the launch's trace; may be null.
    const diagnostics::Trace* trace{};
  };

  /// @enum DatacenterPick
  /// @brief Whether the player may pick the datacenter at launch.
  ///
  /// @var DatacenterPick::as_configured
  /// @brief As the configuration's preference says.
  ///
  /// @var DatacenterPick::never_at_launch
  /// @brief Never; a preference to ask at launch becomes automatic.
  enum class DatacenterPick : std::uint8_t { as_configured, never_at_launch };

  /// @brief Builds the launch from a configuration.
  ///
  /// @param configuration The cloud link's configuration.
  /// @param providers The providers; must outlive the launch.
  /// @param seams Replacements for collaborators; must outlive the launch.
  /// @param listener Told what the launch does; must outlive it.
  /// @param pick Whether the player may pick the datacenter at launch.
  CloudProvisioning(const CloudLinkConfiguration& configuration,
                    Providers providers, CloudLaunchSeams seams,
                    Core::Listener& listener,
                    DatacenterPick pick = DatacenterPick::as_configured);
  /// @brief Not copyable; it owns the launch.
  CloudProvisioning(const CloudProvisioning&) = delete;
  /// @brief Not copy-assignable; it owns the launch.
  auto operator=(const CloudProvisioning&) -> CloudProvisioning& = delete;

  /// @brief Guesses the service a configuration will stream on, without
  /// blocking.
  ///
  /// @param configuration The configuration.
  /// @param seams The seams; the clearance is asked when given.
  /// @return The allocation's service when one is given; CloudStream without a
  ///     product; otherwise the launch's guess.
  static auto expected_service(const CloudLinkConfiguration& configuration,
                               const CloudLaunchSeams& seams)
      -> cloud::CloudService;
  /// @brief Turns a reserved server into an allocation a cloud link can
  /// connect to directly.
  ///
  /// @param slot The reserved server.
  /// @return The allocation, with the selected datacenter's MTUs and round
  ///     trip (0 when unknown) and the picker rows.
  static auto allocation_of(play::ServerSlot slot) -> CloudAllocation;
  /// @brief Names the venue of a cloud service.
  ///
  /// @param service LegacyCloudStream or CloudStream.
  /// @return The cloud venue on the matching service generation.
  static auto venue_of(cloud::CloudService service) noexcept -> Venue;
  /// @brief Tells whether a cloud server streaming a platform's game has a
  /// system menu the XMB toggle opens.
  ///
  /// @param platform The platform the reserved server streams.
  /// @return `true` only for a PS3 game; a PS4 game on LegacyCloudStream has
  ///     no server menu and a PS5 game takes the PS button itself.
  static auto has_system_menu(cloud::CloudPlatform platform) noexcept -> bool;
  /// @brief Finds the cloud launch of a link.
  ///
  /// @param link A link.
  /// @return The launch of a cloud link that provisions; null otherwise.
  static auto launch_of(Link& link) noexcept -> Core*;

  /// @brief Gives the launch.
  ///
  /// @return The launch; null when the configuration cannot launch.
  auto core() noexcept -> Core* { return core_.get(); }
  /// @brief Reads the service the launch streams on.
  ///
  /// @return The launch's service; CloudStream without a launch.
  auto service() const noexcept -> cloud::CloudService;
  /// @brief Checks what can be checked before anything is sent.
  ///
  /// @return The precondition the configuration failed, or the launch's own;
  ///     empty when sound.
  auto precondition() const -> std::optional<Core::Precondition>;
  /// @brief Runs the launch to a reserved server; blocks on PS, the
  /// measurements, the player's pick and the queue.
  ///
  /// @return The reserved server; an unmet precondition or the launch's
  ///     failure.
  [[nodiscard]] auto run() -> Expected<play::ServerSlot, play::LaunchFailure>;

 private:
  /// @brief Applies the datacenter pick rule to the configuration's
  /// preference.
  ///
  /// @param configuration The configuration.
  /// @param pick The rule.
  /// @return The preference to launch with.
  static auto preference_of(const CloudLinkConfiguration& configuration,
                            DatacenterPick pick) -> play::DatacenterPreference;

  /// @brief The clearance over the configuration's catalog, when no seam
  /// gives one.
  std::optional<play::CatalogClearance> catalog_clearance_;
  /// @brief The clearance asked; null when there is none.
  play::TitleClearance* clearance_{};
  /// @brief Waits on sockets when no waiter seam is given.
  net::http::SleepWaiter sleep_waiter_;
  /// @brief The system time zone, used unless a seam replaces it.
  platform::SystemUtcOffset system_utc_offset_;
  /// @brief A datacenter view over the configuration's store, when no seam
  /// gives one.
  std::optional<play::Datacenters> own_datacenters_;
  /// @brief The datacenter view used; null to keep none.
  play::Datacenters* datacenters_{};
  /// @brief Why the configuration cannot launch; empty when it can.
  std::optional<Core::Precondition> unusable_;
  /// @brief The launch; null when the configuration cannot launch.
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play
