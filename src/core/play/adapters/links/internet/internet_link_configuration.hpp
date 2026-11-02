/// @file
/// @brief Declares InternetLinkConfiguration, everything a link to a console
/// over the internet starts from.
#pragma once

#include "src/core/play/adapters/links/link.hpp"

#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/nat/holepunch/remote_defaults.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace libchiaking::users {
class PsUser;
}

namespace libchiaking::play {

/// @brief The settings of an internet link: the user and console, the
/// picture and controller, NAT port guessing, the start timeout and the stop
/// token.
///
/// Built by InternetLinks::configured and checked by
/// InternetLinkConfiguration::validated; a value moved into the link.
struct InternetLinkConfiguration {
  /// @brief The most NAT ports guessed.
  static constexpr std::size_t port_guess_limit = 80;
  /// @brief The most NAT probe sockets.
  static constexpr std::size_t nat_probe_socket_limit = 4096;
  /// @brief How long an internet start may take by default.
  static constexpr std::chrono::seconds default_start_timeout{60};

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

  /// @brief Checks the configuration.
  ///
  /// @return The configuration; StreamError::invalid_configuration without a
  ///     PSN user, for a target that is neither PS4 10 nor PS5 1, an unknown
  ///     codec or feedback mode, a zero timeout, too many port guesses or probe
  ///     sockets, or invalid tuning.
  [[nodiscard]] auto validated() && -> Expected<
      InternetLinkConfiguration, net::protocol::remote_play::StreamError>;

  /// @brief The user to play as; required.
  std::shared_ptr<users::PsUser> user;
  /// @brief The console's PSN device id.
  std::array<std::byte, psn_device_uid_size> device_uid{};
  /// @brief The registered console: its target and keys.
  net::protocol::remote_play::RegisteredConsole console;
  /// @brief The client's device id sent to the console; all zero to generate
  /// one.
  std::array<std::byte, rp_did_size> did{};
  /// @brief The console's host, when known.
  std::string host;
  /// @brief The console's control port.
  std::uint16_t ctrl_port{rp_session_port};
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
  bool enable_keyboard{false};
  /// @brief Whether to ask for a keyframe after a frame FEC cannot repair.
  bool enable_idr_on_fec_failure{false};
  /// @brief Whether IPv6 candidates are offered.
  bool enable_ipv6{false};
  /// @brief Whether NAT ports are guessed whatever the NAT.
  bool force_port_guessing{false};
  /// @brief How many NAT ports are guessed.
  std::size_t port_guess_count{net::nat::holepunch::default_port_guess_count};
  /// @brief How many sockets probe the NAT.
  std::size_t nat_probe_socket_count{
      net::nat::holepunch::default_nat_probe_socket_count};
  /// @brief How long the start may take, in microseconds.
  std::uint64_t start_timeout_us{
      net::nat::holepunch::default_remote_start_timeout_us};
  /// @brief Developer tuning, audio channels and the share tap.
  net::protocol::remote_play::SessionTuning tuning{};
  /// @brief Cancels the link.
  platform::StopToken stop;
};

}  // namespace libchiaking::play
