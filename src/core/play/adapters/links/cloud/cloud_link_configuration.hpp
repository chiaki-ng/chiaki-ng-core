/// @file
/// @brief Declares CloudLinkConfiguration, everything a cloud link starts
/// from, and CloudAllocation, a server already reserved that a link can
/// connect to without launching.
#pragma once

#include "src/core/play/adapters/links/link.hpp"

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/remote_play/video_presets.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"
#include "src/core/psn/gk/launch_spec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::users {
class PsUser;
}

namespace libchiaking::catalog {
class Catalog;
}

namespace libchiaking::play {
class DatacenterStore;
}

namespace libchiaking::platform {
class UtcOffsetProvider;
}

namespace libchiaking::play {

/// @brief The picture a cloud stream asks for when none is given: 1080p.
inline constexpr VideoPreset default_cloud_video_preset = video_preset_1080p;

/// @brief A reserved cloud server a link connects to directly, skipping the
/// launch. Secrets are held in wiping strings. Plain data checked by
/// CloudAllocation::valid.
struct CloudAllocation {
  /// @brief The longest launch specification accepted, in bytes.
  static constexpr std::size_t launch_spec_limit = 64 * bytes_per_kib;

  /// @brief Checks the allocation.
  ///
  /// @return `true` for a known service and platform, a server address, a
  ///     handshake key that decodes, and a launch specification within
  ///     CloudAllocation::launch_spec_limit without NUL.
  auto valid() const noexcept -> bool;

  /// @brief The service that reserved it.
  cloud::CloudService service{cloud::CloudService::cloud_stream};
  /// @brief The platform the title streams as.
  cloud::CloudPlatform platform{cloud::CloudPlatform::ps5};
  /// @brief The entitlement launched.
  std::string entitlement_id;
  /// @brief The server's address.
  std::string server_ip;
  /// @brief The server's port.
  std::uint16_t server_port{};
  /// @brief The handshake key, encoded as PS sent it.
  SecretString handshake_key;
  /// @brief The launch specification sent to the server.
  SecretString launch_spec;
  /// @brief PS's session id.
  SecretString session_id;
  /// @brief The PSN wrapper type of the session.
  std::uint8_t psn_wrapper_type{1};
  /// @brief The measured inbound path MTU, in bytes; 0 when unknown.
  std::uint32_t mtu_in{};
  /// @brief The measured outbound path MTU, in bytes; 0 when unknown.
  std::uint32_t mtu_out{};
  /// @brief The measured round trip, in microseconds; 0 when unknown.
  std::uint64_t rtt_us{};
  /// @brief The datacenter rows shown to the player.
  std::vector<cloud::DatacenterPing> datacenter_pings;
};

/// @brief Collaborators a test may replace; declared in cloud_link.hpp.
struct CloudLaunchSeams;

/// @brief The settings of a cloud link: who plays what, where, at what
/// picture, and with which controller, or an allocation to connect to.
///
/// Built by CloudLinkConfiguration::create from a CloudLinkRequest and
/// checked by CloudLinkConfiguration::validated; a value moved into the
/// link. The catalog and datacenter store it points at must outlive the
/// link.
struct CloudLinkConfiguration {
  /// @brief The longest time zone name accepted, in bytes.
  static constexpr std::size_t locale_text_limit = 64;

  /// @brief Applies the controller preference.
  ///
  /// The DualSense control channel is used for anything but a DualShock 4.
  ///
  /// @param preference The preference.
  /// @return Nothing; StreamError::invalid_configuration for a value past
  ///     DualSense.
  [[nodiscard]] auto prefer_controller(
      net::protocol::remote_play::ControllerPreference preference) noexcept
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (preference >
        net::protocol::remote_play::ControllerPreference::dualsense)
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_configuration);
    enable_dualsense_ctrl =
        preference !=
        net::protocol::remote_play::ControllerPreference::dualshock4;
    return {};
  }

  /// @brief Builds the configuration, deriving the time zone from the device's
  /// UTC offset when the request gives none.
  ///
  /// @param launch The request.
  /// @param clock Supplies the UTC offset.
  /// @return The configuration; ConfigurationError::invalid_credential without
  ///     a PSN account or catalog; ConfigurationError::time_zone_out_of_range
  ///     for an offset PS does not accept;
  ///     ConfigurationError::clock_unavailable when the offset cannot be read;
  ///     otherwise as the other overload.
  [[nodiscard]] static auto create(CloudLinkRequest launch,
                                   const platform::UtcOffsetProvider& clock)
      -> Expected<CloudLinkConfiguration, ConfigurationError>;
  /// @brief Builds the configuration from a request that names its time zone.
  ///
  /// @param launch The request.
  /// @return The configuration; ConfigurationError::invalid_credential without
  ///     a PSN account or catalog; ConfigurationError::clock_unavailable
  ///     without a time zone; ConfigurationError::time_zone_out_of_range for a
  ///     name that is not canonical; ConfigurationError::rejected when the
  ///     controller or the result fails validation.
  [[nodiscard]] static auto create(CloudLinkRequest launch)
      -> Expected<CloudLinkConfiguration, ConfigurationError>;
  /// @brief Checks the configuration.
  ///
  /// @return The configuration; StreamError::invalid_configuration for an
  ///     invalid allocation, a launch without account, catalog or product, a
  ///     time zone too long, with NUL or missing for a launch, a bitrate that
  ///     is not positive, an unknown feedback mode or invalid tuning.
  [[nodiscard]] auto validated() && -> Expected<
      CloudLinkConfiguration, net::protocol::remote_play::StreamError>;
  /// @brief Builds the cloud link with the real collaborators.
  ///
  /// @param configuration The checked configuration.
  /// @param platform The host's providers; must outlive the link.
  /// @return The link; StreamError::provider_unavailable without crypto or
  ///     HTTP providers.
  [[nodiscard]] static auto link(CloudLinkConfiguration configuration,
                                 platform::PlatformFactory& platform)
      -> Expected<std::unique_ptr<Link>,
                  net::protocol::remote_play::StreamError>;
  /// @brief Builds the cloud link with replaced collaborators.
  ///
  /// @param configuration The checked configuration.
  /// @param platform The host's providers; must outlive the link.
  /// @param seams Replacements; must outlive the link.
  /// @return The link; StreamError::provider_unavailable without crypto or
  ///     HTTP providers.
  [[nodiscard]] static auto link(CloudLinkConfiguration configuration,
                                 platform::PlatformFactory& platform,
                                 CloudLaunchSeams seams)
      -> Expected<std::unique_ptr<Link>,
                  net::protocol::remote_play::StreamError>;

  /// @brief The user to play as.
  std::shared_ptr<users::PsUser> user;
  /// @brief The catalog that clears the title; must outlive the link.
  catalog::Catalog* catalog{};
  /// @brief The title; empty only with an allocation.
  std::optional<catalog::ProductId> product;
  /// @brief What the start already cleared.
  std::optional<catalog::ClearedLaunch> cleared;
  /// @brief Where measured datacenters are kept; null to keep none.
  play::DatacenterStore* datacenters{};
  /// @brief Whether to skip the LegacyCloudStream account attributes check.
  bool skip_account_attributes_check{};
  /// @brief The time zone name sent with the launch.
  std::string time_zone;
  /// @brief Which datacenter to use, or how to choose one.
  DatacenterPreference datacenter;
  /// @brief Prior datacenter measurements to use instead of the remembered
  /// ones.
  std::vector<cloud::DatacenterPing> prior_datacenters;
  /// @brief The picture height asked for, in lines.
  int resolution{static_cast<int>(default_cloud_video_preset.height)};
  /// @brief The bitrate asked for, in kilobits per second.
  int bitrate_kbps{static_cast<int>(default_cloud_video_preset.bitrate_kbps)};
  /// @brief Whether HDR is asked for.
  bool hdr{};
  /// @brief Whether to measure the bandwidth to the server.
  bool measure_bandwidth{};
  /// @brief Whether the DualSense control channel is used.
  bool enable_dualsense_ctrl{true};
  /// @brief Whether to ask for a keyframe after a frame FEC cannot repair.
  bool enable_idr_on_fec_failure{};
  /// @brief How controller feedback is delivered.
  net::protocol::remote_play::FeedbackMode feedback{
      net::protocol::remote_play::FeedbackMode::standard};
  /// @brief Developer tuning and the audio channels asked for.
  net::protocol::remote_play::SessionTuning tuning{};
  /// @brief The player's screen reader and contrast choices for the cloud
  /// console.
  psn::gk::Accessibility accessibility{};
  /// @brief Whether PS5 cloud favours picture quality over steady
  /// performance.
  bool high_quality{true};
  /// @brief A server to connect to without launching; empty to launch.
  std::optional<CloudAllocation> allocation;
  /// @brief Cancels the link.
  platform::StopToken stop;

 private:
  /// @brief Checks the request has what a launch needs to sign in.
  ///
  /// @param launch The request.
  /// @return `true` with a user that has a PSN account and a catalog.
  static auto credentials_valid(const CloudLinkRequest& launch) noexcept
      -> bool;
};

}  // namespace libchiaking::play
