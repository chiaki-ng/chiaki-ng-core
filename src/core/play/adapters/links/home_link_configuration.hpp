/// @file
/// @brief Declares HomeLinkConfiguration, everything a link to a paired
/// console on the local network starts from.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/direct_route.hpp"
#include "src/core/play/adapters/links/link.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace libchiaking::consoles {
class Console;
}

namespace libchiaking::users {
class PsUser;
}

namespace libchiaking::play {

enum class ConfigurationError : std::uint8_t;
class VideoRequest;
class PortGuessing;
struct ConsolePreferences;
struct CloudLinkRequest;

/// @brief The settings of a home link: the registered console, its address
/// and ports, the picture and controller, and the stop token.
///
/// Built by HomeLinkConfiguration::create from a paired console and checked
/// by HomeLinkConfiguration::validated; a value moved into the link.
struct HomeLinkConfiguration {
  /// @brief The longest host name or address accepted, in bytes.
  static constexpr std::size_t host_limit = 255;

  /// @brief Applies the controller preference.
  ///
  /// The DualSense control channel is used for a DualSense, or for the
  /// console's default on a PS5.
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
        preference ==
            net::protocol::remote_play::ControllerPreference::dualsense ||
        (preference == net::protocol::remote_play::ControllerPreference::
                           console_default &&
         console.target ==
             net::protocol::remote_play::RegistrationTarget::ps5_1);
    return {};
  }

  /// @brief Builds the configuration for playing a paired console at home.
  ///
  /// @param console The console.
  /// @param as The user it is paired with.
  /// @param video The picture.
  /// @param preferences Controller, feedback, keyboard, keyframe and tuning.
  /// @return The configuration; ConfigurationError::invalid_host without an
  ///     address; ConfigurationError::invalid_console for a forgotten console;
  ///     ConfigurationError::not_paired when not paired for home play;
  ///     ConfigurationError::rejected when the controller preference or the
  ///     result fails validation.
  [[nodiscard]] static auto create(const consoles::Console& console,
                                   const users::PsUser& as, VideoRequest video,
                                   ConsolePreferences preferences)
      -> Expected<HomeLinkConfiguration, ConfigurationError>;
  /// @brief Checks the configuration.
  ///
  /// @return The configuration; StreamError::invalid_configuration for an
  ///     empty, oversized or NUL-containing host, an unknown target, codec or
  ///     feedback mode, a zero port, or invalid tuning.
  [[nodiscard]] auto validated() && -> Expected<
      HomeLinkConfiguration, net::protocol::remote_play::StreamError>;
  /// @brief Builds the home link.
  ///
  /// @param configuration The checked configuration.
  /// @param platform The host's providers; must outlive the link.
  /// @return The link; StreamError::provider_unavailable without a crypto
  ///     provider.
  [[nodiscard]] static auto link(HomeLinkConfiguration configuration,
                                 platform::PlatformFactory& platform)
      -> Expected<std::unique_ptr<Link>,
                  net::protocol::remote_play::StreamError>;

  /// @brief The registered console: its target and keys.
  net::protocol::remote_play::RegisteredConsole console;
  /// @brief The console's host name or address.
  std::string host;
  /// @brief The control endpoint; an unspecified address is resolved from
  /// HomeLinkConfiguration::host.
  platform::SocketEndpoint ctrl_endpoint{};
  /// @brief The stream route's endpoints and socket options; the remote
  /// address is filled in once the console is resolved.
  net::protocol::remote_play::DirectRouteConfig route{};
  /// @brief The console's stream port.
  std::uint16_t active_stream_port{rp_stream_port};
  /// @brief The client's device id sent to the console; all zero to generate
  /// one at start.
  std::array<std::byte, rp_did_size> did{};
  /// @brief The codec asked for.
  net::protocol::remote_play::VideoCodec codec{
      net::protocol::remote_play::VideoCodec::h264};
  /// @brief The picture asked for.
  net::protocol::remote_play::LocalVideoSettings video{};
  /// @brief How controller feedback is delivered.
  net::protocol::remote_play::FeedbackMode feedback{
      net::protocol::remote_play::FeedbackMode::standard};
  /// @brief Whether the DualSense control channel is used.
  bool enable_dualsense_ctrl{};
  /// @brief Whether the console's keyboard prompt is handled.
  bool enable_keyboard{};
  /// @brief Whether to ask for a keyframe after a frame FEC cannot repair.
  bool enable_idr_on_fec_failure{};
  /// @brief Developer tuning, audio channels and the share tap.
  net::protocol::remote_play::SessionTuning tuning{};
  /// @brief Cancels the link.
  platform::StopToken stop;
};

}  // namespace libchiaking::play
