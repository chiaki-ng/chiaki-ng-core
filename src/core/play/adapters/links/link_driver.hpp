/// @file
/// @brief Declares LinkDriver, which owns a session's link and serialises
/// every call into it between the session thread and frontend threads.
#pragma once

#include "libchiaking/detail/play/keyboard_state.hpp"
#include "src/core/play/adapters/links/link.hpp"

#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/datacenters/datacenter_selector.hpp"

#include "libchiaking/detail/secret_string.hpp"

#include <rigtorp/MPMCQueue.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::play {

/// @brief Receives what a link delivers, with video already turned into
/// frames; called on the thread pumping the link.
class LinkDriverSink {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~LinkDriverSink() = default;
  /// @brief Takes one event other than a video unit.
  ///
  /// @param event The event.
  virtual void on_event(net::protocol::remote_play::StreamEvent event) = 0;
  /// @brief Takes one video frame.
  ///
  /// @param frame The frame.
  virtual void on_video_frame(media::VideoFrame frame) = 0;
};

/// @brief Owns a session's link and lets one thread at a time into it.
///
/// The session thread starts, pumps, waits on and stops the link, holding the
/// link's turn while it does. An input call from another thread runs at once
/// when the turn is free; otherwise, or from inside the link's own callback,
/// it is queued and run at the start of the next pump. A stop asked for from
/// inside a callback is deferred until the callback returns. The link's state
/// is published after every turn so other threads read it without the turn;
/// its diagnostics are read on request, inside a turn. Not copyable.
///
/// Queued inputs are concrete commands held in a lock-free ring allocated
/// once at construction, so queueing a controller state or a microphone frame
/// never allocates or locks. Inputs run in the order they were queued, and a
/// caller that takes the turn runs everything already queued before its own
/// input, so one thread's inputs reach the link in the order it sent them.
class LinkDriver final : public DatacenterSelector, private LinkEventSink {
 public:
  /// @brief The most inputs queued for the next pump.
  static constexpr std::size_t max_pending_inputs = 4096;
  /// @brief The size of one microphone frame, in bytes.
  static constexpr std::size_t microphone_frame_size = 40;

  /// @brief Takes ownership of a link and sets itself as its event sink.
  ///
  /// @param session The link; must not be null, or std::invalid_argument is
  ///     thrown.
  /// @param stop Stops the link; requested on cancel, stop and destruction.
  /// @param threading Provider of the driver's mutexes; must outlive the
  ///     driver.
  /// @param sink Receives the link's events; must outlive the driver.
  /// @param log Receives log lines; must outlive the driver.
  /// @param venue Where the link runs, for the start message.
  LinkDriver(std::unique_ptr<Link> session, platform::StopSource stop,
             platform::ThreadingProvider& threading, LinkDriverSink& sink,
             platform::DiagnosticsProvider& log,
             std::optional<Venue> venue = std::nullopt);
  /// @brief Requests the stop and releases the link.
  ~LinkDriver() override;

  /// @brief Not copyable; the driver owns the link.
  LinkDriver(const LinkDriver&) = delete;
  /// @brief Not copy-assignable; the driver owns the link.
  auto operator=(const LinkDriver&) -> LinkDriver& = delete;

  /// @brief Starts the link, blocking while it connects; then runs a stop
  /// deferred during the start.
  ///
  /// @return Nothing; the link's error; StreamError::invalid_state when called
  ///     from inside the link's callback or when the link is stopping after
  ///     the start.
  auto start() -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Runs queued inputs, then one round of the link's work, then a
  /// deferred stop.
  ///
  /// @return Nothing; the link's error or the stop's;
  ///     StreamError::invalid_state from inside the link's callback.
  auto pump_once() -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Blocks on the link until it has work or the time passes; returns
  /// at once when a stop was requested or inputs are queued.
  ///
  /// @param timeout The longest wait.
  /// @return Nothing; the link's error; StreamError::invalid_state from inside
  ///     the link's callback.
  auto wait(std::chrono::milliseconds timeout)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Stops the link; blocks while it closes. From inside the link's
  /// callback the stop is deferred instead.
  ///
  /// @return Nothing, also when deferred; the link's error;
  ///     StreamError::invalid_state when called while holding the turn outside
  ///     a callback.
  auto stop() -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Stops the link because the host is going to sleep; as
  /// LinkDriver::stop otherwise.
  ///
  /// @return As LinkDriver::stop.
  auto stop_for_sleep()
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Requests the stop and wakes the link; never blocks.
  void cancel() noexcept;
  /// @brief Wakes the link; may be called from any thread.
  void wake() noexcept;
  /// @brief Passes the player's datacenter pick to the link.
  ///
  /// @param name The datacenter name.
  /// @return Nothing; StreamError::invalid_argument for an empty name, one
  ///     longer than DatacenterName::limit or one containing a NUL; otherwise
  ///     the link's answer.
  auto select_datacenter(std::string_view name)
      -> Expected<void, net::protocol::remote_play::StreamError> override;

  /// @brief Reads the link's state as last published.
  ///
  /// @return The state.
  auto state() const noexcept -> net::protocol::remote_play::StreamState;
  /// @brief Reads the link's diagnostics now.
  ///
  /// Takes the link's turn unless the calling thread holds it, so a call from
  /// another thread waits out a pump or wait in progress.
  ///
  /// @return A copy of the diagnostics.
  auto diagnostics() -> net::protocol::remote_play::StreamDiagnostics;
  /// @brief Reads the link's first failure.
  ///
  /// @return The failure; empty when none.
  auto failure() const noexcept -> const std::optional<LinkFailure>&;
  /// @brief Reads the link's first cleanup failure.
  ///
  /// @return The failure; empty when none.
  auto cleanup_failure() const noexcept -> const std::optional<LinkFailure>&;
  /// @brief Reads the link's quit reason.
  ///
  /// @return The reason and text.
  auto quit() const noexcept -> const net::protocol::remote_play::QuitDetail&;
  /// @brief Reads the link's Takion version.
  ///
  /// @return The version; empty before it is known.
  auto takion_version() const noexcept -> std::optional<std::uint32_t>;
  /// @brief Reads the link's ECDH curve.
  ///
  /// @return The curve; empty before it is known.
  auto ecdh_curve() const noexcept -> std::optional<platform::EcdhCurve>;

  /// @brief Sends a controller state, now or with the next pump.
  ///
  /// @param state The state.
  /// @return The link's result when run now; nothing when queued;
  ///     StreamError::invalid_state when max_pending_inputs are already
  ///     queued.
  auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends the login code, kept in wiping storage until sent.
  ///
  /// @param pin The code's bytes.
  /// @return As LinkDriver::send_controller_state.
  auto submit_pin(std::span<const std::byte> pin)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Reports lost video frames, now or with the next pump.
  ///
  /// @param first The first lost frame index.
  /// @param last The last lost frame index.
  /// @return As LinkDriver::send_controller_state.
  auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends a Create button press to a cloud service, now or with the
  /// next pump.
  ///
  /// @param press How the button was pressed.
  /// @return As LinkDriver::send_controller_state.
  auto send_create(CloudMessaging::CapturePress press)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends a raw deep link to a cloud service, now or with the next
  /// pump.
  ///
  /// @param text The deep link.
  /// @return As LinkDriver::send_controller_state.
  auto send_deep_link(std::string text)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends a game invitation to a cloud service, now or with the next
  /// pump.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return As LinkDriver::send_controller_state.
  auto send_invitation(std::string invitation_id, std::string session_id)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends a microphone frame, now or with the next pump.
  ///
  /// @param opus_frame The encoded frame.
  /// @return As LinkDriver::send_controller_state.
  auto send_microphone(
      std::span<const std::byte, microphone_frame_size> opus_frame)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Connects the microphone, now or with the next pump.
  ///
  /// @return As LinkDriver::send_controller_state.
  auto connect_microphone()
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Opens or closes a cloud service's system menu, now or with the
  /// next pump.
  ///
  /// @param pad The pressing controller's pad, 0 for player one.
  /// @return As LinkDriver::send_controller_state.
  auto toggle_system_menu(std::uint32_t pad)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Opens a LegacyCloudStream service's saved data screen, now or with
  /// the next pump.
  ///
  /// @return As LinkDriver::send_controller_state.
  auto open_saved_data()
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Mutes or unmutes the microphone, now or with the next pump.
  ///
  /// @param muted Whether to mute.
  /// @return As LinkDriver::send_controller_state.
  auto set_microphone_muted(bool muted)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Asks for a keyframe, now or with the next pump.
  ///
  /// @return As LinkDriver::send_controller_state.
  auto request_idr() -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Discards the next video frame, now or with the next pump.
  ///
  /// @return As LinkDriver::send_controller_state.
  auto drop_video_frame()
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends an audio state message, now or with the next pump.
  ///
  /// @param kind Which audio state.
  /// @param body Its payload, copied.
  /// @return As LinkDriver::send_controller_state.
  auto send_audio_state(net::protocol::remote_play::AudioStateKind kind,
                        std::span<const std::byte> body)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends the keyboard's state, now or with the next pump.
  ///
  /// @param state The keys down.
  /// @return As LinkDriver::send_controller_state.
  auto send_keyboard(const KeyboardState& state)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Runs a console command, now or with the next pump.
  ///
  /// @param command The command.
  /// @return As LinkDriver::send_controller_state.
  auto execute(net::protocol::remote_play::StreamCommand command)
      -> Expected<void, net::protocol::remote_play::StreamError>;

 private:
  /// @brief The result of a call into the link.
  using Result = Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief A queued controller state.
  struct ControllerInput {
    /// @brief The state.
    net::protocol::remote_play::ControllerState state;
  };
  /// @brief A queued login code, in wiping storage.
  struct PinInput {
    /// @brief The code's bytes.
    SecretString pin;
  };
  /// @brief A queued video loss report.
  struct VideoLossInput {
    /// @brief The first lost frame index.
    std::uint16_t first{};
    /// @brief The last lost frame index.
    std::uint16_t last{};
  };
  /// @brief A queued Create button press.
  struct CreateInput {
    /// @brief How the button was pressed.
    CloudMessaging::CapturePress press{};
  };
  /// @brief A queued deep link.
  struct DeepLinkInput {
    /// @brief The deep link.
    std::string text;
  };
  /// @brief A queued game invitation.
  struct InvitationInput {
    /// @brief The invitation's id.
    std::string invitation_id;
    /// @brief The invited game session's id.
    std::string session_id;
  };
  /// @brief A queued microphone frame, held in place.
  struct MicrophoneInput {
    /// @brief The encoded frame.
    std::array<std::byte, microphone_frame_size> frame{};
  };
  /// @brief A queued microphone connection.
  struct ConnectMicrophoneInput {};
  /// @brief A queued system menu toggle.
  struct SystemMenuInput {
    /// @brief The pressing controller's pad.
    std::uint32_t pad{};
  };
  /// @brief A queued request for the saved data screen.
  struct SavedDataInput {};
  /// @brief A queued microphone mute change.
  struct MuteInput {
    /// @brief Whether to mute.
    bool muted{};
  };
  /// @brief A queued keyframe request.
  struct KeyframeInput {};
  /// @brief A queued request to discard the next video frame.
  struct DropFrameInput {};
  /// @brief A queued audio state message.
  struct AudioStateInput {
    /// @brief Which audio state.
    net::protocol::remote_play::AudioStateKind kind{};
    /// @brief Its payload.
    std::vector<std::byte> body;
  };
  /// @brief A queued keyboard state.
  struct KeyboardInput {
    /// @brief The keys down.
    KeyboardState state;
  };
  /// @brief A queued console command.
  struct CommandInput {
    /// @brief The command.
    net::protocol::remote_play::StreamCommand command;
  };
  /// @brief A call into the link, run now or queued for the next pump.
  using Input =
      std::variant<ControllerInput, PinInput, VideoLossInput, CreateInput,
                   DeepLinkInput, InvitationInput, MicrophoneInput,
                   ConnectMicrophoneInput, SystemMenuInput, SavedDataInput,
                   MuteInput, KeyframeInput, DropFrameInput, AudioStateInput,
                   KeyboardInput, CommandInput>;
  /// @brief One slot of the input ring; empty only while unused.
  using QueuedInput = std::optional<Input>;

  /// @brief Holds the link's turn for a scope and publishes its state when it
  /// ends; defined in the implementation.
  class Turn;
  /// @brief Counts nested callbacks from the link for a scope; defined in the
  /// implementation.
  class Dispatch;
  /// @enum DeferredStop
  /// @brief A stop asked for from inside a callback.
  ///
  /// @var DeferredStop::none
  /// @brief None.
  ///
  /// @var DeferredStop::stop
  /// @brief A stop.
  ///
  /// @var DeferredStop::sleep
  /// @brief A stop for host sleep, which wins over a plain stop.
  enum class DeferredStop : std::uint8_t { none, stop, sleep };

  /// @brief Refuses a missing link.
  ///
  /// @param session The link.
  /// @return @p session; throws std::invalid_argument when it is null.
  static auto require(std::unique_ptr<Link> session) -> std::unique_ptr<Link>;

  /// @brief Publishes state changes and passes events on, turning video units
  /// into frames.
  ///
  /// @param event The link's event.
  void on_event(net::protocol::remote_play::StreamEvent event) override;

  /// @brief Runs a call into the link now when the turn is free and the caller
  /// is not inside a callback, after the inputs already queued; otherwise,
  /// or when a queued input could not be run yet because its push is still
  /// in progress, queues it behind them and wakes the link.
  ///
  /// @param input The call.
  /// @return The call's result when run now; nothing when queued;
  ///     StreamError::invalid_state when the queue is full.
  auto submit(Input input) -> Result;
  /// @brief Runs one input against the link; the turn must be held.
  ///
  /// @param input The input.
  /// @return The link's result.
  auto run(Input& input) -> Result;
  /// @brief Reports whether the calling thread holds the link's turn.
  ///
  /// @return `true` inside a turn on this thread.
  auto owns_session() const noexcept -> bool;
  /// @brief Copies the link's state for other threads.
  void publish() noexcept;
  /// @brief Runs every input queued so far, oldest first, at most
  /// max_pending_inputs of them; failures are logged once. Inputs a callback
  /// queues while they run may run in the same drain.
  void drain_inputs();
  /// @brief Drops every queued input.
  void discard_inputs() noexcept;
  /// @brief Stops the link, or defers the stop when inside a callback.
  ///
  /// @param sleep Whether the stop is for host sleep.
  /// @return As LinkDriver::stop.
  auto request_stop(bool sleep)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Stops the link with the turn held, then drops queued inputs.
  ///
  /// @param sleep Whether the stop is for host sleep.
  /// @return The link's result.
  auto run_stop(bool sleep)
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Defers a stop asked for inside a callback and wakes the link.
  ///
  /// @param sleep Whether the stop is for host sleep.
  /// @return `true` when deferred; `false` outside a callback.
  auto defer_nested_stop(bool sleep) -> bool;
  /// @brief Runs a deferred stop.
  ///
  /// @return Nothing when none was deferred; the stop's result.
  auto finish_deferred_stop()
      -> Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Stops the link.
  platform::StopSource stop_;
  /// @brief Receives the link's events.
  LinkDriverSink& sink_;
  /// @brief Receives log lines.
  platform::DiagnosticsProvider& log_;
  /// @brief The line logged when the link starts.
  std::string_view start_message_;
  /// @brief The link's state as last published.
  std::atomic<net::protocol::remote_play::StreamState> published_state_{
      net::protocol::remote_play::StreamState::idle};
  /// @brief How deep the link's callbacks are nested.
  std::atomic<int> dispatch_depth_{};
  /// @brief The stop deferred until a callback returns.
  std::atomic<DeferredStop> deferred_stop_{DeferredStop::none};
  /// @brief The link's turn.
  platform::Mutex session_mutex_;
  /// @brief The thread holding the turn; empty when free.
  std::atomic<platform::ThreadIdentity> session_owner_{};
  /// @brief Inputs queued for the next pump, oldest first; any thread
  /// pushes, only the turn's holder pops.
  rigtorp::MPMCQueue<QueuedInput> inputs_;
  /// @brief The link.
  std::unique_ptr<Link> session_;
};

}  // namespace libchiaking::play
