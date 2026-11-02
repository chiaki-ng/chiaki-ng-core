/// @file
/// @brief Declares Link, the connection a session drives to a console or a
/// cloud server, with LinkEnding, the record of how it ended, and
/// LinkEventSink, the receiver of its events.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/keyboard_state.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "src/core/play/adapters/links/driven_link.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/link_inputs.hpp"
#include "src/core/play/adapters/links/link_readings.hpp"
#include "src/core/play/model/venue.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::play {

class ActiveStream;

/// @brief Receives the stream events a link produces; called on the thread
/// that pumps the link.
class LinkEventSink {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~LinkEventSink() = default;
  /// @brief Takes one event.
  ///
  /// @param event The event.
  virtual void on_event(net::protocol::remote_play::StreamEvent event) = 0;
};

/// @brief The record of why a link ended: the first failure, the first
/// failure while cleaning up, and the first reason the other end gave for
/// quitting.
///
/// Each is kept once; later ones are ignored. Failures are stamped with the
/// venue they happened on. Not synchronised; owned by one link and touched on
/// its thread.
class LinkEnding {
 public:
  /// @brief Builds an empty record.
  ///
  /// @param venue Where the link runs; empty when unknown.
  explicit LinkEnding(std::optional<Venue> venue) noexcept : venue_(venue) {}

  /// @brief Keeps the first failure, stamped with the venue, and its quit
  /// reason.
  ///
  /// @param failure The failure.
  void record_failure(LinkFailure failure) noexcept;
  /// @brief Keeps the first failure met while cleaning up, stamped with the
  /// venue.
  ///
  /// @param failure The failure.
  void record_cleanup_failure(LinkFailure failure) noexcept;
  /// @brief Keeps the first quit reason other than none.
  ///
  /// @param reason Why the other end quit.
  /// @param text Its text, cut to the protocol's limit.
  void record_quit(net::protocol::remote_play::QuitReason reason,
                   std::string_view text = {}) noexcept;
  /// @brief Builds the event that tells a link stopped, recording a stop as
  /// the quit reason when none was recorded.
  ///
  /// @return The state change to stopped with the recorded quit.
  auto stopped_event() noexcept
      -> net::protocol::remote_play::event::StateChanged;

  /// @brief Reads the venue.
  ///
  /// @return Where the link runs; empty when unknown.
  auto venue() const noexcept -> std::optional<Venue> { return venue_; }
  /// @brief Reads the first failure.
  ///
  /// @return The failure; empty when none was recorded.
  auto failure() const noexcept -> const std::optional<LinkFailure>& {
    return failure_;
  }
  /// @brief Reads the first cleanup failure.
  ///
  /// @return The failure; empty when none was recorded.
  auto cleanup_failure() const noexcept -> const std::optional<LinkFailure>& {
    return cleanup_failure_;
  }
  /// @brief Reads the quit reason.
  ///
  /// @return The reason and text; QuitReason::none when none was recorded.
  auto quit() const noexcept -> const net::protocol::remote_play::QuitDetail& {
    return quit_;
  }

 private:
  /// @brief Where the link runs.
  std::optional<Venue> venue_;
  /// @brief The first failure.
  std::optional<LinkFailure> failure_;
  /// @brief The first cleanup failure.
  std::optional<LinkFailure> cleanup_failure_;
  /// @brief The first quit reason.
  net::protocol::remote_play::QuitDetail quit_;
};

/// @brief A connection a session drives: to a console at home or over the
/// internet, or to a cloud server.
///
/// The session's thread runs it through its DrivenLink calls and sends it
/// LinkInputs; other threads may only wake it. Most inputs, the wait, the
/// wake and the event sink forward to the link's active stream, and inputs
/// refuse with StreamError::invalid_state when there is none. How the link
/// ended is kept in its LinkEnding.
class Link : public DrivenLink, public LinkInputs, public LinkReadings {
 public:
  /// @brief The result of a link call: nothing, or the stream error.
  using Result = Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Allows destruction through the interface.
  virtual ~Link() = default;
  /// @brief Blocks until the link has work or the time passes.
  ///
  /// @param timeout The longest wait.
  /// @return Nothing; the stream's error. Without a stream it returns at
  ///     once.
  [[nodiscard]] auto wait(std::chrono::milliseconds timeout) -> Result override;
  /// @brief Ends a wait early; may be called from any thread.
  void wake() noexcept override;
  /// @brief Passes the player's datacenter pick to a cloud launch.
  ///
  /// @param name The datacenter name.
  /// @return StreamError::invalid_state by default; a cloud link answers.
  [[nodiscard]] auto select_datacenter(std::string_view name)
      -> Result override;
  /// @brief Stops the link because the host is going to sleep.
  ///
  /// @return As Link::stop by default.
  [[nodiscard]] auto stop_for_sleep() -> Result override { return stop(); }
  /// @brief Reads the stream's state.
  ///
  /// @return The state; StreamState::idle without a stream.
  auto state() const noexcept
      -> net::protocol::remote_play::StreamState override;
  /// @brief Sends one controller state.
  ///
  /// @param state The state.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state)
      -> Result override;
  /// @brief Reports a run of lost video frames to the sender.
  ///
  /// @param first The first lost frame index.
  /// @param last The last lost frame index.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result override;
  /// @brief Runs a command on the console.
  ///
  /// @param command The command.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto execute(
      const net::protocol::remote_play::StreamCommand& command)
      -> Result override;
  /// @brief Sends a Create button press to a cloud service.
  ///
  /// @param press How the button was pressed.
  /// @return The stream's result; StreamError::invalid_configuration on a
  ///     link without cloud messaging.
  [[nodiscard]] auto send_create(CloudMessaging::CapturePress press)
      -> Result override;
  /// @brief Sends a raw deep link to a cloud service.
  ///
  /// @param text The deep link.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_deep_link(std::string text) -> Result override;
  /// @brief Sends a game invitation to a cloud service.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_invitation(std::string invitation_id,
                                     std::string session_id) -> Result override;
  /// @brief Sends one microphone frame.
  ///
  /// @param opus_frame The encoded frame.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_microphone(std::span<const std::byte> opus_frame)
      -> Result override;
  /// @brief Connects the microphone.
  ///
  /// @return StreamError::invalid_state by default.
  [[nodiscard]] auto connect_microphone() -> Result override;
  /// @brief Opens or closes a cloud service's system menu for one pad, as
  /// the PS button does on LegacyCloudStream.
  ///
  /// @param pad The pressing controller's pad, 0 for player one.
  /// @return StreamError::invalid_state by default.
  [[nodiscard]] auto toggle_system_menu(std::uint32_t pad) -> Result override;
  /// @brief Opens a LegacyCloudStream service's Application Saved Data
  /// Management screen.
  ///
  /// @return StreamError::invalid_state by default.
  [[nodiscard]] auto open_saved_data() -> Result override;
  /// @brief Mutes or unmutes the microphone.
  ///
  /// @param muted Whether to mute.
  /// @return StreamError::invalid_state by default.
  [[nodiscard]] auto set_microphone_muted(bool muted) -> Result override;
  /// @brief Asks the sender for a keyframe.
  ///
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto request_idr() -> Result override;
  /// @brief Discards the next video frame as if the network had lost it, to
  /// exercise loss recovery.
  ///
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto drop_video_frame() -> Result override;
  /// @brief Sends an audio state message.
  ///
  /// @param kind Which audio state.
  /// @param body Its payload.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_audio_state(
      net::protocol::remote_play::AudioStateKind kind,
      std::span<const std::byte> body) -> Result override;
  /// @brief Sends the keyboard's state.
  ///
  /// @param state The keys down.
  /// @return The stream's result; StreamError::invalid_state without one.
  [[nodiscard]] auto send_keyboard(const KeyboardState& state)
      -> Result override;
  /// @brief Sets where the link's events go; ignored without a stream.
  ///
  /// @param sink The receiver; must outlive the link.
  void set_event_sink(LinkEventSink& sink) noexcept override;
  /// @brief Reads the stream's diagnostics.
  ///
  /// @return The diagnostics; empty without a stream.
  auto diagnostics() const noexcept
      -> net::protocol::remote_play::StreamDiagnostics override;
  /// @brief Reads the Takion version the stream committed to.
  ///
  /// @return The version; empty before it is known.
  auto takion_version() const noexcept -> std::optional<std::uint32_t> override;

  /// @brief Reads the first failure.
  ///
  /// @return The failure; empty when none was recorded.
  auto failure() const noexcept -> const std::optional<LinkFailure>& {
    return ending_.failure();
  }
  /// @brief Reads the first cleanup failure.
  ///
  /// @return The failure; empty when none was recorded.
  auto cleanup_failure() const noexcept -> const std::optional<LinkFailure>& {
    return ending_.cleanup_failure();
  }
  /// @brief Reads the quit reason.
  ///
  /// @return The reason and text.
  auto quit() const noexcept -> const net::protocol::remote_play::QuitDetail& {
    return ending_.quit();
  }

 protected:
  /// @brief Builds the link with an empty ending.
  ///
  /// @param venue Where the link runs; empty when unknown.
  explicit Link(std::optional<Venue> venue = std::nullopt) noexcept
      : ending_(venue) {}

  /// @brief Gives the ending for the link to record into.
  ///
  /// @return The ending.
  auto ending() noexcept -> LinkEnding& { return ending_; }
  /// @brief Gives the active stream the default members forward to.
  ///
  /// @return The stream; null by default.
  virtual auto active_stream() noexcept -> ActiveStream* { return nullptr; }
  /// @brief Gives the active stream the default members forward to.
  ///
  /// @return The stream; null by default.
  virtual auto active_stream() const noexcept -> const ActiveStream* {
    return nullptr;
  }

 private:
  /// @brief The refusal of a call without a stream.
  ///
  /// @return StreamError::invalid_state.
  static auto refused() -> Result;

  /// @brief How the link ended.
  LinkEnding ending_;
};

}  // namespace libchiaking::play
