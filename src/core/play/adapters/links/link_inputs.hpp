/// @file
/// @brief Declares LinkInputs, the inputs a session sends into a running
/// link.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/keyboard_state.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace libchiaking::play {

/// @brief The inputs a session sends into a running link: controller, pin,
/// microphone, keyboard, video recovery, console commands and cloud service
/// messages.
///
/// The link driver runs each one with the link's turn held, at once or
/// queued for the next pump; a reconnecting link passes each to its current
/// attempt.
class LinkInputs {
 public:
  /// @brief The result of an input: nothing, or the stream error.
  using Result = Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Allows destruction through the interface.
  virtual ~LinkInputs() = default;

  /// @brief Sends one controller state.
  ///
  /// @param state The state.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state) -> Result = 0;
  /// @brief Sends the login code the console asked for.
  ///
  /// @param pin The code's bytes.
  /// @return Nothing; the error otherwise.
  [[nodiscard]] virtual auto submit_pin(std::span<const std::byte> pin)
      -> Result = 0;
  /// @brief Reports a run of lost video frames to the sender.
  ///
  /// @param first The first lost frame index.
  /// @param last The last lost frame index.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto report_video_loss(std::uint16_t first,
                                               std::uint16_t last)
      -> Result = 0;
  /// @brief Runs a command on the console.
  ///
  /// @param command The command.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto execute(
      const net::protocol::remote_play::StreamCommand& command) -> Result = 0;
  /// @brief Sends a Create button press to a cloud service.
  ///
  /// @param press How the button was pressed.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_create(CloudMessaging::CapturePress press)
      -> Result = 0;
  /// @brief Sends a raw deep link to a cloud service.
  ///
  /// @param text The deep link.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_deep_link(std::string text) -> Result = 0;
  /// @brief Sends a game invitation to a cloud service.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_invitation(std::string invitation_id,
                                             std::string session_id)
      -> Result = 0;
  /// @brief Sends one microphone frame.
  ///
  /// @param opus_frame The encoded frame.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_microphone(
      std::span<const std::byte> opus_frame) -> Result = 0;
  /// @brief Connects the microphone.
  ///
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto connect_microphone() -> Result = 0;
  /// @brief Opens or closes a cloud service's system menu for one pad, as
  /// the PS button does on LegacyCloudStream.
  ///
  /// @param pad The pressing controller's pad, 0 for player one.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto toggle_system_menu(std::uint32_t pad)
      -> Result = 0;
  /// @brief Opens a LegacyCloudStream service's Application Saved Data
  /// Management screen.
  ///
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto open_saved_data() -> Result = 0;
  /// @brief Mutes or unmutes the microphone.
  ///
  /// @param muted Whether to mute.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto set_microphone_muted(bool muted) -> Result = 0;
  /// @brief Asks the sender for a keyframe.
  ///
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto request_idr() -> Result = 0;
  /// @brief Discards the next video frame as if the network had lost it, to
  /// exercise loss recovery.
  ///
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto drop_video_frame() -> Result = 0;
  /// @brief Sends an audio state message.
  ///
  /// @param kind Which audio state.
  /// @param body Its payload.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_audio_state(
      net::protocol::remote_play::AudioStateKind kind,
      std::span<const std::byte> body) -> Result = 0;
  /// @brief Sends the keyboard's state.
  ///
  /// @param state The keys down.
  /// @return Nothing; the stream's error.
  [[nodiscard]] virtual auto send_keyboard(const KeyboardState& state)
      -> Result = 0;
};

}  // namespace libchiaking::play
