/// @file
/// @brief Declares ActiveStream, the part every link shares once the other
/// end is reached: the stream state, the media session, controller delivery,
/// the couch protocol, the waits and how the stream fails or stops.
#pragma once

#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/net/protocol/remote_play/stream/local_start.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "src/core/play/adapters/links/link.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "src/core/play/adapters/session/seats_console.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::play {

/// @brief The running stream of one link, whether home, internet or cloud.
///
/// The link builds it with its profile, hands it the calibrated pipeline or
/// the active session once connected, and pumps it. It keeps the public
/// stream state and emits every event to the link's sink, delivers media and
/// feedback, sends controller states through a delivery once streaming, runs
/// couch and control commands, and records failures and stops in the link's
/// ending. Controller states sent before streaming are stored and the first
/// is submitted when streaming begins. Not synchronised: the link calls it
/// from the session thread, inside the link driver's turn; only
/// ActiveStream::wake may come from elsewhere. Owned by its link; the owner,
/// ending and platform must outlive it. Not copyable.
class ActiveStream {
 public:
  /// @brief The result of a stream call.
  using Result = Link::Result;
  /// @brief The console-side couch protocol over the active session.
  using Couch = Seats::Core::Console<net::protocol::remote_play::ActiveSession>;
  /// @brief The controller delivery over the active session.
  using Delivery = net::protocol::remote_play::ControllerDelivery<
      net::protocol::remote_play::ActiveSession>;

  /// @brief Waits on a stream's sockets with the platform poller.
  ///
  /// While a route is watched or a stream is ticked the wait is cut to
  /// protocol_tick so timers keep running; a scheduled stream waits the whole
  /// timeout its caller bounded by its deadlines. Without a poller it sleeps
  /// at most 1 ms. The worker thread waits; any thread may call wake().
  class RouteWaiter {
   public:
    /// @brief The most sockets one wait watches.
    static constexpr std::size_t max_pollables = 4;

    /// @brief How a wait is cut short for timers.
    enum class Pacing : std::uint8_t {
      /// @brief Nothing is running; only a watched socket cuts the wait to
      /// protocol_tick.
      idle,
      /// @brief The stream runs timers it does not schedule; the wait is cut
      /// to protocol_tick.
      ticked,
      /// @brief Every timer is in the timeout; the wait is not cut.
      scheduled,
    };

    /// @brief Shortens a timeout so it ends at a deadline.
    ///
    /// @param timeout The longest wait.
    /// @param due_us The deadline in monotonic microseconds, if any.
    /// @param now_us Monotonic microseconds now.
    /// @return The shorter of the two; zero when the deadline has passed.
    static auto bound_to_deadline(std::chrono::nanoseconds timeout,
                                  std::optional<std::uint64_t> due_us,
                                  std::uint64_t now_us) noexcept
        -> std::chrono::nanoseconds;

    /// @brief The longest wait while a stream has protocol timers to run (4
    /// ms).
    static constexpr std::chrono::milliseconds protocol_tick{4};

    /// @brief Makes a waiter.
    ///
    /// @param network The network provider whose poller is used; a failure to
    ///     make one leaves the waiter sleeping instead.
    /// @param threading Provider of the sleeper used without a poller; must
    ///     outlive the waiter.
    /// @param clock Monotonic clock the sleeper measures on; must outlive the
    ///     waiter.
    /// @param stop Ends any wait.
    RouteWaiter(platform::NetworkProvider& network,
                platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock, platform::StopToken stop);

    /// @brief Waits for one route to become readable.
    ///
    /// Blocks for at most `timeout`, or protocol_tick when a route is given or
    /// a route is being established.
    ///
    /// @param timeout The longest wait.
    /// @param route The route to watch, or null.
    /// @param establishing Whether a route is being established.
    void wait(std::chrono::nanoseconds timeout, const platform::Pollable* route,
              bool establishing = false);

    /// @brief Waits for any of several sockets to become readable.
    ///
    /// Blocks for at most `timeout`, cut to protocol_tick as `pacing` says.
    ///
    /// @param timeout The longest wait.
    /// @param watched Up to four sockets, each named by its position; null
    ///     entries are skipped and repeats share one request.
    /// @param pacing How the wait is cut short for timers.
    /// @return A bit per position whose socket the poll found readable, 0 when
    ///     none was; nothing when no poll ran, for no time left, a stop, a
    ///     failed poll or no poller.
    auto wait(std::chrono::nanoseconds timeout,
              std::initializer_list<const platform::Pollable*> watched,
              Pacing pacing) -> std::optional<std::uint32_t>;

    /// @brief Waits for any of several sockets to become readable.
    ///
    /// Blocks for at most `timeout`, or protocol_tick when anything is watched
    /// or the stream is active.
    ///
    /// @param timeout The longest wait.
    /// @param watched Up to four sockets, each named by its position; null
    ///     entries are skipped and repeats share one request.
    /// @param active Whether the stream is active.
    /// @return A bit per position whose socket became readable; 0 on a timeout,
    ///     stop or wake.
    auto wait(std::chrono::nanoseconds timeout,
              std::initializer_list<const platform::Pollable*> watched,
              bool active) -> std::uint32_t;

    /// @brief Ends a wait in progress, from any thread.
    void wake() noexcept;

   private:
    /// @brief Ends any wait.
    platform::StopToken stop_;
    /// @brief The poller, or null when none could be made.
    std::unique_ptr<platform::Poller> poller_;
    /// @brief Sleeps through a wait when there is no poller.
    platform::Sleeper sleeper_;
  };

  /// @brief What the stream needs back from its link.
  class Owner {
   public:
    /// @brief Allows destruction through the interface.
    virtual ~Owner() = default;
    /// @brief Stops the link, as when the console shut the stream down.
    ///
    /// @return The link's stop result.
    [[nodiscard]] virtual auto stop() -> Result = 0;
    /// @brief Releases the link's route after a failure.
    ///
    /// @return `true` when released cleanly.
    [[nodiscard]] virtual auto release_route() -> bool = 0;
  };

  /// @brief How a link's stream behaves. Plain data.
  struct Profile {
    /// @brief The diagnostic component it traces as.
    diagnostics::Component component;
    /// @brief Which couch protocol the other end speaks.
    Seats::Core::ConsoleKind console;
    /// @brief How controller feedback is delivered.
    net::protocol::remote_play::FeedbackMode feedback{
        net::protocol::remote_play::FeedbackMode::standard};
    /// @brief Whether the link has a control channel; a cloud link has none.
    bool has_ctrl{true};
    /// @brief The cloud gateway addresses to receive on; empty when the
    /// service has no cloud messaging.
    std::vector<std::uint32_t> cloud_receivers{};
  };

  /// @enum Progress
  /// @brief How a media pump went.
  ///
  /// @var Progress::running
  /// @brief The media keeps running.
  ///
  /// @var Progress::stopped
  /// @brief The media closed in the ordinary way.
  ///
  /// @var Progress::failed
  /// @brief The media failed.
  enum class Progress : std::uint8_t { running, stopped, failed };

  /// @brief Builds an idle stream.
  ///
  /// @param owner The link; must outlive the stream.
  /// @param ending The link's ending; must outlive the stream.
  /// @param platform The host's providers; must outlive the stream.
  /// @param stop Stops waits and the trace.
  /// @param profile How the stream behaves.
  ActiveStream(Owner& owner, LinkEnding& ending,
               platform::PlatformFactory& platform, platform::StopToken stop,
               Profile profile);
  /// @brief Not copyable; the stream owns its media.
  ActiveStream(const ActiveStream&) = delete;
  /// @brief Not copy-assignable; the stream owns its media.
  auto operator=(const ActiveStream&) -> ActiveStream& = delete;
  /// @brief Releases the media.
  ~ActiveStream();

  /// @brief Sets where events go.
  ///
  /// @param sink The receiver; must outlive the stream.
  void set_sink(LinkEventSink& sink) noexcept { sink_ = &sink; }
  /// @brief Reports whether events have somewhere to go.
  ///
  /// @return `true` once a sink is set.
  auto has_sink() const noexcept -> bool { return sink_ != nullptr; }
  /// @brief Traces an event and passes it to the sink.
  ///
  /// @param event The event.
  void emit(net::protocol::remote_play::StreamEvent event);
  /// @brief Moves to a state and emits the change.
  ///
  /// @param state The state.
  void enter(net::protocol::remote_play::StreamState state);
  /// @brief Passes the console's picture prohibition to the control features,
  /// emitting what they produce.
  ///
  /// @param prohibited Whether the console blocks the picture.
  void prohibit(bool prohibited);
  /// @brief Drains control notices into couch events, abandons couch joins
  /// that timed out while streaming, then emits the events.
  ///
  /// @tparam Drain A callable taking a vector of events and a Couch::Binding.
  /// @param drain Fills the events and settles notices through the binding.
  template <class Drain>
  void deliver_notices(Drain&& drain) {
    std::vector<net::protocol::remote_play::StreamEvent> events;
    {
      auto couch = couch_backend();
      drain(events, couch);
      if (streaming()) couch.tick(events, features_);
    }
    for (auto& event : events) emit(std::move(event));
  }

  /// @brief Reads the state.
  ///
  /// @return The stream state.
  auto state() const noexcept -> net::protocol::remote_play::StreamState {
    return state_;
  }
  /// @brief Reports whether the stream is connecting or streaming.
  ///
  /// @return `true` while establishing the route, negotiating or streaming.
  auto running() const noexcept -> bool;
  /// @brief Reports whether the stream is streaming.
  ///
  /// @return `true` while streaming.
  auto streaming() const noexcept -> bool {
    return state_ == net::protocol::remote_play::StreamState::streaming;
  }
  /// @brief Reports whether the stream ended.
  ///
  /// @return `true` once stopped or failed.
  auto ended() const noexcept -> bool;
  /// @brief Moves to a link stage and traces its beginning.
  ///
  /// @param stage The stage.
  void at(net::protocol::remote_play::FailureStage stage) noexcept;
  /// @brief Reads the stage.
  ///
  /// @return The current stage.
  auto stage() const noexcept -> net::protocol::remote_play::FailureStage {
    return stage_;
  }
  /// @brief Gives the trace.
  ///
  /// @return The trace, owned by the stream.
  auto trace() noexcept -> SessionDiagnostics& { return trace_; }
  /// @brief Gives the link's ending.
  ///
  /// @return The ending.
  auto ending() noexcept -> LinkEnding& { return ending_; }
  /// @brief Gives the couch protocol.
  ///
  /// @return The couch, owned by the stream.
  auto couch() noexcept -> Couch& { return couch_; }
  /// @brief Gives the control features.
  ///
  /// @return The features, owned by the stream.
  auto features() noexcept -> net::protocol::remote_play::CtrlFeatures& {
    return features_;
  }
  /// @brief Reports whether the control channel is open.
  ///
  /// @return `true` until closed, when the profile has one.
  auto ctrl_open() const noexcept -> bool { return ctrl_open_; }
  /// @brief Marks the control channel closed.
  void close_ctrl() noexcept { ctrl_open_ = false; }
  /// @brief Ends a wait early; may be called from any thread.
  void wake() noexcept { waiter_.wake(); }
  /// @brief Idles the thread for up to a time, waking early when woken.
  ///
  /// @param remaining The longest idle.
  void idle(std::chrono::microseconds remaining);

  /// @brief Takes the calibrated pipeline of a console link.
  ///
  /// @param pipeline The pipeline.
  void calibrate(
      std::unique_ptr<net::protocol::remote_play::LocalCalibratedPipeline>
          pipeline);
  /// @brief Takes the active session of a link without a pipeline.
  ///
  /// @param session The session.
  void connect(
      std::unique_ptr<net::protocol::remote_play::ActiveSession> session);
  /// @brief Sets the route waits poll.
  ///
  /// @param route The route's pollable; must outlive its use.
  void route_through(const platform::Pollable* route) noexcept {
    route_ = route;
  }
  /// @brief Reports whether a pipeline or session is held.
  ///
  /// @return `true` once connected.
  auto has_media() const noexcept -> bool;
  /// @brief Gives the calibrated pipeline.
  ///
  /// @return The pipeline; null when none.
  auto pipeline() noexcept
      -> net::protocol::remote_play::LocalCalibratedPipeline*;
  /// @brief Gives the active session.
  ///
  /// @return The pipeline's session or the held one; null when none.
  auto active() noexcept -> net::protocol::remote_play::ActiveSession*;
  /// @brief Gives the active session.
  ///
  /// @return The pipeline's session or the held one; null when none.
  auto active() const noexcept
      -> const net::protocol::remote_play::ActiveSession*;
  /// @brief Traces the active session's handshake state and health, and
  /// emits event::ConnectionWaiting when the playing stream starts or stops
  /// going connection_waiting_notice_us without receiving anything.
  void observe();
  /// @brief Pumps the pipeline or session once.
  ///
  /// @return Progress::stopped or Progress::failed when it ended, keeping the
  ///     pipeline's error; Progress::running otherwise.
  auto pump_media() -> Progress;
  /// @brief Reads the last pipeline pump error.
  ///
  /// @return The error; empty when the last pump did not fail.
  auto pump_error() const noexcept
      -> std::optional<net::protocol::remote_play::LocalPipelineError> {
    return pump_error_;
  }
  /// @brief Starts streaming once negotiating and the session's description is
  /// ready: builds the controller delivery, submits the stored first state and
  /// emits the description and the state change.
  ///
  /// @param launch What the console or server granted, when known.
  /// @param note A line to log as streaming begins; empty for none.
  /// @return Nothing, also when not yet ready; the couch's error when the
  ///     first controller state was refused.
  [[nodiscard]] auto stream_when_ready(
      std::optional<net::protocol::remote_play::event::EffectiveLaunch> launch,
      std::string_view note = {}) -> Result;
  /// @brief Refreshes controller delivery and delivers media.
  void keep_streaming();
  /// @brief Emits up to a queue's worth of media and feedback units.
  void deliver_media();
  /// @brief Drops the delivery and route and asks the media to stop.
  ///
  /// @return `true` when it stopped cleanly or there was nothing; `false`
  ///     after recording a cleanup failure.
  [[nodiscard]] auto release_media() -> bool;
  /// @brief Keeps a failure met while switching transports, so it explains a
  /// later pipeline failure.
  ///
  /// @param failure The failure.
  void note_transition_failure(LinkFailure failure) {
    transition_failure_ = std::move(failure);
  }
  /// @brief Reads the transport switch failure.
  ///
  /// @return The failure; empty when none.
  auto transition_failure() const noexcept
      -> const std::optional<LinkFailure>& {
    return transition_failure_;
  }

  /// @brief Sends a controller state, or stores it before streaming.
  ///
  /// @param state The state.
  /// @return Nothing; StreamError::controller_rejected when invalid or
  ///     refused; StreamError::invalid_state while stopping or ended.
  [[nodiscard]] auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state) -> Result;
  /// @brief Reports lost video frames to the sender.
  ///
  /// @param first The first lost frame index.
  /// @param last The last lost frame index.
  /// @return Nothing; StreamError::invalid_state when not streaming; the
  ///     failure when the report could not be posted, which fails the stream.
  [[nodiscard]] auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result;
  /// @brief Sends an addressed cloud gateway message.
  ///
  /// @param address The gateway address.
  /// @param version The message version.
  /// @param body The body.
  /// @return Nothing; StreamError::invalid_configuration when the service
  ///     has no cloud messaging; StreamError::invalid_state when not
  ///     streaming or the service has not listed the address yet.
  [[nodiscard]] auto send_cloud_message(std::uint32_t address,
                                        std::uint32_t version,
                                        std::vector<std::byte> body) -> Result;
  /// @brief Sends a raw deep link to the cloud service, numbered in order.
  ///
  /// @param text The deep link, such as
  ///     `psaction:systemservicestatus?isSystemUiOverlaid=1`.
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_deep_link(std::string text) -> Result;
  /// @brief Sends a game invitation to the cloud service as the typed INVITE
  /// deep link, numbered in order with the raw
  /// deep links.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_invitation(std::string invitation_id,
                                     std::string session_id) -> Result;
  /// @brief Connects the microphone to a PS5 cloud service.
  ///
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_microphone_connection() -> Result;
  /// @brief Opens or closes a LegacyCloudStream service's system menu for one
  /// pad.
  ///
  /// @param pad The pressing controller's pad, 0 for player one.
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_system_menu_toggle(std::uint32_t pad) -> Result;
  /// @brief Opens a LegacyCloudStream service's saved data screen with the
  /// typed SAVE_DATA deep link.
  ///
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_saved_data() -> Result;
  /// @brief Tells a PS5 cloud service the microphone is muted or unmuted.
  ///
  /// @param muted Whether the microphone is muted.
  /// @return Nothing; StreamError::invalid_state when not streaming.
  [[nodiscard]] auto send_microphone_mute(bool muted) -> Result;
  /// @brief Sends a microphone frame.
  ///
  /// @param opus_frame The encoded frame.
  /// @return Nothing; StreamError::invalid_state when not streaming;
  ///     StreamError::invalid_configuration for the wrong size; the failure
  ///     when it could not be posted, which fails the stream.
  [[nodiscard]] auto send_microphone(std::span<const std::byte> opus_frame)
      -> Result;
  /// @brief Sends an audio state message.
  ///
  /// @param kind Which audio state.
  /// @param data Its payload.
  /// @return Nothing; StreamError::invalid_state when not streaming;
  ///     StreamError::invalid_configuration when the session does not support
  ///     it; StreamError::pipeline_failed when it could not be posted.
  [[nodiscard]] auto send_audio_state(
      net::protocol::remote_play::AudioStateKind kind,
      std::span<const std::byte> data) -> Result;
  /// @brief Sends the keyboard's state as a history event of seat 0, through
  /// the controller delivery so it shares the button edges' numbering and
  /// resends.
  ///
  /// @param state The keys down.
  /// @return Nothing once the event is queued, as a refused window is sent
  ///     with the next refresh; StreamError::invalid_state when not streaming;
  ///     otherwise as ActiveStream::send_audio_state.
  [[nodiscard]] auto send_keyboard(const KeyboardState& state) -> Result;
  /// @brief Asks the sender for a keyframe.
  ///
  /// @return Nothing; StreamError::invalid_state when not streaming;
  ///     StreamError::pipeline_failed when it could not be posted.
  [[nodiscard]] auto request_idr() -> Result;
  /// @brief Discards the next video frame as if the network had lost it, to
  /// exercise loss recovery.
  ///
  /// @return Nothing; StreamError::invalid_state when not streaming or not
  ///     ready.
  [[nodiscard]] auto drop_video_frame() -> Result;
  /// @brief Runs a command: pad count and account ids at any time, control
  /// commands while the channel is open, couch commands while running.
  ///
  /// @param command The command.
  /// @return Nothing; StreamError::invalid_state when not running or the
  ///     channel is closed; the couch's or features' error.
  [[nodiscard]] auto execute(
      const net::protocol::remote_play::StreamCommand& command) -> Result;
  /// @brief Blocks until the route or another pollable is readable, the media's
  /// next deadline, the time passes or a wake; pumps the media when its
  /// deadline passed.
  ///
  /// When the network's socket calls are costly, a streaming wait ends at the
  /// earliest timer of the session, its Takion client and the controller
  /// delivery instead of every protocol tick, and what the poll found on the
  /// route is passed to the session so its next pump reads only a readable
  /// socket.
  ///
  /// @param timeout The longest wait.
  /// @param watched Extra pollables, at most three.
  /// @return Nothing.
  [[nodiscard]] auto wait(
      std::chrono::milliseconds timeout,
      std::initializer_list<const platform::Pollable*> watched) -> Result;
  /// @brief Takes whether the last wait found one of its extra pollables
  /// readable, and marks it as not readable until the next wait reports.
  ///
  /// @param index The pollable's position among the last wait's extras.
  /// @return `true` when that wait found it readable, when no wait has
  ///     reported, or when the network's socket calls are cheap, so the
  ///     caller reads without asking.
  [[nodiscard]] auto take_watched_readable(std::size_t index) noexcept -> bool;
  /// @brief Reads the active session's diagnostics.
  ///
  /// @return The diagnostics with deferred delivery refreshes counted as
  ///     dropped posts and the controller delivery's counters in the input
  ///     section; empty without a session.
  auto diagnostics() const noexcept
      -> net::protocol::remote_play::StreamDiagnostics;
  /// @brief Reads the Takion version the media committed to.
  ///
  /// @return The version; empty before it is known.
  auto committed_takion_version() const noexcept
      -> std::optional<std::uint32_t>;

  /// @brief Fails the stream: records the explained failure, releases the
  /// route, emits the failure.
  ///
  /// A pipeline failure the console caused by shutting down stops the link
  /// instead.
  ///
  /// @param error The stream error.
  /// @param reason The public failure reason.
  /// @param detail The failure's detail, when known.
  /// @return @p error, or the owner's stop result for a console shutdown.
  [[nodiscard]] auto fail(net::protocol::remote_play::StreamError error,
                          net::protocol::remote_play::StreamFailure reason,
                          std::optional<LinkFailure> detail = {}) -> Result;
  /// @brief Records that a stop was asked for.
  ///
  /// @return `true` while the stream has not ended.
  [[nodiscard]] auto stopping() -> bool;
  /// @brief Finishes a stop.
  ///
  /// @param cleaned Whether the media released cleanly.
  /// @return Nothing and the stopped state; StreamError::shutdown_failed and
  ///     the failed state when it did not.
  [[nodiscard]] auto stopped(bool cleaned) -> Result;
  /// @brief Reports whether stopping failed to clean up.
  ///
  /// @return `true` after an unclean stop.
  auto shutdown_failed() const noexcept -> bool { return shutdown_failed_; }

 private:
  /// @brief Nothing yet, the calibrated pipeline, or the active session.
  using Media = std::variant<
      std::monostate,
      std::unique_ptr<net::protocol::remote_play::LocalCalibratedPipeline>,
      std::unique_ptr<net::protocol::remote_play::ActiveSession>>;

  /// @brief Gives where media comes from.
  ///
  /// @return The pipeline's media source or the active session; null when
  ///     none.
  auto media_source() noexcept -> net::protocol::remote_play::ActiveSession*;
  /// @brief Gives the active session while streaming.
  ///
  /// @return The session; null when not streaming.
  auto streaming_session() noexcept
      -> net::protocol::remote_play::ActiveSession*;
  /// @brief Binds the couch to this pass's delivery, session and time.
  ///
  /// @return The binding.
  auto couch_backend() -> Couch::Binding;
  /// @brief Gives the media's loss, unless a transport switch failure explains
  /// it.
  ///
  /// @return The failure; null when none.
  auto media_loss() const noexcept
      -> const net::protocol::remote_play::ActiveSessionFailure*;
  /// @brief Logs a peer disconnect, delivers what is left, and reports
  /// whether the console ended the stream on purpose, recording that quit.
  ///
  /// @return `true` for a console shutdown, a reached streaming time limit or
  ///     a game trial ended after its time warning.
  auto ended_by_console() -> bool;
  /// @brief Logs a time-left warning at info level with the server's own
  /// reason text, so an unfamiliar reason can be read in the log.
  ///
  /// @param left The warning.
  void record_play_time_left(
      const net::protocol::takion::PlayTimeLeft& left) noexcept;
  /// @brief Logs an unrecognised message at debug level, with up to 256 of
  /// its bytes in hex.
  ///
  /// @param message The message.
  void record_unrecognised(
      const net::protocol::remote_play::UnrecognisedMessage& message) noexcept;
  /// @brief Finds the failure that best explains a stream error.
  ///
  /// @param error The stream error.
  /// @param detail The failure's detail, when known.
  /// @return The detail, or for a pipeline failure with media the switch
  ///     failure, the media's loss or the calibration failure.
  auto explained(net::protocol::remote_play::StreamError error,
                 std::optional<LinkFailure> detail) const -> LinkFailure;
  /// @brief Turns a session post's result into a stream result.
  ///
  /// @param active The session.
  /// @param result The post's result.
  /// @return Nothing; StreamError::invalid_configuration for an unsupported
  ///     request; a stream failure when the session failed;
  ///     StreamError::pipeline_failed otherwise.
  [[nodiscard]] auto posted(
      net::protocol::remote_play::ActiveSession& active,
      const Expected<void, net::protocol::remote_play::ActiveSessionError>&
          result) -> Result;
  /// @brief Picks how a wait is cut short for timers.
  ///
  /// @param source The media source, or null.
  /// @return Scheduled while streaming on the lean path, ticked while
  ///     running, idle otherwise.
  auto pacing(const net::protocol::remote_play::ActiveSession* source)
      const noexcept -> RouteWaiter::Pacing;
  /// @brief Reads the monotonic time.
  ///
  /// @return Microseconds.
  auto now_us() const noexcept -> std::uint64_t {
    return clock_.now_microseconds();
  }

  /// @brief How long the stream may receive nothing while it plays before the
  /// player is told the connection is waiting, in microseconds (5 s).
  static constexpr std::uint64_t connection_waiting_notice_us = 5'000'000;
  /// @brief How long a stop on the lean path keeps the session open for its
  /// DISCONNECT to be acknowledged.
  static constexpr std::chrono::milliseconds lean_disconnect_drain{1000};
  /// @brief The most media units delivered per pass.
  static constexpr std::size_t frames_per_delivery =
      net::protocol::remote_play::ActiveMediaQueue::capacity;

  /// @brief The link.
  Owner& owner_;
  /// @brief The link's ending.
  LinkEnding& ending_;
  /// @brief The host's providers.
  platform::PlatformFactory& platform_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief The trace.
  SessionDiagnostics trace_;
  /// @brief The couch protocol.
  Couch couch_;
  /// @brief The control features.
  net::protocol::remote_play::CtrlFeatures features_;
  /// @brief How feedback is delivered.
  net::protocol::remote_play::FeedbackDeliveryPolicy feedback_;
  /// @brief Whether the network's socket calls are costly, which selects the
  /// lean path.
  const bool lean_;
  /// @brief What the last wait's poll found, a bit per position with the route
  /// first; empty when no poll has reported.
  std::optional<std::uint32_t> readable_;
  /// @brief Waits on the route and wakes.
  RouteWaiter waiter_;
  /// @brief The pipeline or session.
  Media media_;
  /// @brief Controller delivery; present while streaming.
  std::optional<Delivery> delivery_;
  /// @brief The route's pollable; null when none.
  const platform::Pollable* route_{};
  /// @brief Where events go; null until set.
  LinkEventSink* sink_{};
  /// @brief The stream state.
  net::protocol::remote_play::StreamState state_{
      net::protocol::remote_play::StreamState::idle};
  /// @brief The current link stage.
  net::protocol::remote_play::FailureStage stage_{
      net::protocol::remote_play::FailureStage::unknown};
  /// @brief The last pipeline pump error.
  std::optional<net::protocol::remote_play::LocalPipelineError> pump_error_;
  /// @brief The transport switch failure.
  std::optional<LinkFailure> transition_failure_;
  /// @brief Delivery refreshes that could not be posted.
  std::uint64_t deferred_refreshes_{};
  /// @brief Whether the control channel is open.
  bool ctrl_open_{};
  /// @brief Whether stopping failed to clean up.
  bool shutdown_failed_{};
  /// @brief The request number of the next deep link.
  std::uint32_t deep_link_requests_{};
  /// @brief The cloud gateway messaging; present on a service that has it.
  std::optional<CloudMessaging> messaging_;
  /// @brief Whether sending the gateway address handshake has been tried.
  bool messaging_opened_{};
  /// @brief Whether the server warned during this stream that a game
  /// trial's play time was running out.
  bool trial_warned_{};
  /// @brief Whether event::ConnectionWaiting last said nothing was arriving.
  bool connection_waiting_{};

  /// @brief Sends the gateway address handshake once streaming.
  void open_messaging();
  /// @brief Hands a received generic data transfer to the gateway messaging.
  ///
  /// @param message The transfer.
  /// @return Whether it was a gateway message.
  auto receive_cloud_transfer(
      const net::protocol::remote_play::UnrecognisedMessage& message) -> bool;
  /// @brief Sends a gateway transfer.
  ///
  /// @param transfer The transfer.
  /// @return As posted().
  auto send_cloud_transfer(CloudMessaging::Transfer transfer) -> Result;
};

}  // namespace libchiaking::play
