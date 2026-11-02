/// @file
/// @brief Declares ReconnectingLink, the console link that opens a fresh link
/// for each attempt, waking the console and resting between attempts as
/// StartPolicy allows, and the reconnection machine behind it.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/links/link.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/model/start_policy.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <boost/sml.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::play::reconnection {

/// @brief Where a reconnecting link stands, as it reads its machine.
enum class Stage : std::uint8_t {
  /// @brief No attempt was opened yet.
  unstarted,
  /// @brief An attempt's link runs.
  attempting,
  /// @brief A failed attempt's link is being stopped before the next.
  retiring,
  /// @brief Resting before opening the next attempt.
  resting,
  /// @brief The last allowed attempt runs; its end is the link's end.
  final_attempt,
  /// @brief The running link is being stopped for good.
  parking,
  /// @brief The link was stopped and released.
  parked,
  /// @brief Stopped while no attempt ran.
  halted,
  /// @brief The opener could not build an attempt's link.
  unopened,
};

/// @brief Attempt state: nothing opened. The initial state.
struct Unstarted {};
/// @brief Attempt state: an attempt runs.
struct Attempting {};
/// @brief Attempt state: a failed attempt is being retired.
struct Retiring {};
/// @brief Attempt state: resting between attempts.
struct Resting {};
/// @brief Attempt state: the last attempt runs.
struct FinalAttempt {};
/// @brief Attempt state: stopping for good.
struct Parking {};
/// @brief Attempt state: stopped and released.
struct Parked {};
/// @brief Attempt state: stopped between attempts.
struct Halted {};
/// @brief Attempt state: an attempt could not be opened.
struct Unopened {};

/// @brief Played state: no attempt has streamed yet. The initial state.
struct NotYetPlayed {};
/// @brief Played state: an attempt streamed, so a later failure is a
/// reconnection.
struct Played {};

/// @brief Event: an attempt's link was opened.
struct AttemptOpened {};
/// @brief Event: the opener could not build a link.
struct OpenRefused {};
/// @brief Event: the policy gave up; the current attempt is the last.
struct GiveUp {};
/// @brief Event: the policy allows another attempt.
struct TryAgain {};
/// @brief Event: the attempt's link was released.
struct AttemptRetired {};
/// @brief Event: the link is being stopped for good.
struct Park {
  /// @brief The running link's state when it was stopped.
  net::protocol::remote_play::StreamState last;
};
/// @brief Event: a stop arrived while no attempt ran.
struct Halt {};
/// @brief Event: an attempt reached streaming.
struct Streamed {
  /// @brief When it did, in monotonic microseconds.
  std::uint64_t at_us{};
};
/// @brief Event: the current attempt failed.
struct AttemptFailed {
  /// @brief When it did, in monotonic microseconds.
  std::uint64_t at_us{};
  /// @brief Whether it failed because this machine had no network.
  bool offline{};
};

/// @brief What the machine's actions write.
struct Context {
  /// @brief The running link's state when it was parked; idle until then.
  net::protocol::remote_play::StreamState parked{
      net::protocol::remote_play::StreamState::idle};
  /// @brief When the current attempt first streamed, in monotonic
  /// microseconds; empty until it does, and cleared as the next opens.
  std::optional<std::uint64_t> attempt_streamed_us{};
  /// @brief When the first of the failures in a row that found this machine
  /// without network happened, in monotonic microseconds; cleared by a
  /// failure that reached the network and by a stream.
  std::optional<std::uint64_t> offline_since_us{};
};

/// @brief The transition table of a reconnecting link.
///
/// The attempt region opens attempts, retires failed ones to rest before the
/// next, makes the attempt the policy gives up on the final one, and parks
/// the running link on a stop; a stop between attempts, or after parking a
/// link that never started, halts, and a failed attempt keeps when this
/// machine was first found without network. The played region records the
/// first stream, and both regions keep when the current attempt first
/// streamed. Stateless.
struct Machine {
  /// @brief Builds the transition table for Boost.SML.
  ///
  /// @return The table of both regions.
  auto operator()() const noexcept {
    using namespace boost::sml;
    const auto remember = [](const Park& event, Context& context) {
      context.parked = event.last;
    };
    const auto opened = [](Context& context) {
      context.attempt_streamed_us.reset();
    };
    const auto streamed = [](const Streamed& event, Context& context) {
      if (!context.attempt_streamed_us)
        context.attempt_streamed_us = event.at_us;
      context.offline_since_us.reset();
    };
    const auto failed = [](const AttemptFailed& event, Context& context) {
      if (!event.offline)
        context.offline_since_us.reset();
      else if (!context.offline_since_us)
        context.offline_since_us = event.at_us;
    };
    const auto parked_unstarted = [](const Context& context) {
      return context.parked == net::protocol::remote_play::StreamState::idle;
    };
    return make_transition_table(
        *state<Unstarted> + event<AttemptOpened> / opened = state<Attempting>,
        state<Resting> + event<AttemptOpened> / opened = state<Attempting>,
        state<Unstarted> + event<OpenRefused> = state<Unopened>,
        state<Resting> + event<OpenRefused> = state<Unopened>,
        state<Attempting> + event<AttemptFailed> / failed,
        state<Attempting> + event<GiveUp> = state<FinalAttempt>,
        state<Attempting> + event<TryAgain> = state<Retiring>,
        state<Retiring> + event<AttemptRetired> = state<Resting>,
        state<Attempting> + event<Park> / remember = state<Parking>,
        state<FinalAttempt> + event<Park> / remember = state<Parking>,
        state<Parking> + event<AttemptRetired> = state<Parked>,
        state<Unstarted> + event<Halt> = state<Halted>,
        state<Resting> + event<Halt> = state<Halted>,
        state<Parked> + event<Halt>[parked_unstarted] = state<Halted>,

        *state<NotYetPlayed> + event<Streamed> / streamed = state<Played>,
        state<Played> + event<Streamed> / streamed);
  }
};

/// @brief The progress of one reconnecting link.
///
/// Owned by the link and used on the session thread; not synchronised. Not
/// copyable.
class Progress {
 public:
  /// @brief Builds the machine in its initial states.
  Progress() : machine_(context_) {}
  /// @brief Not copyable; the machine refers to its own context.
  Progress(const Progress&) = delete;
  /// @brief Not copy-assignable; the machine refers to its own context.
  auto operator=(const Progress&) -> Progress& = delete;

  /// @brief Feeds an event to the machine.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return `true` when a transition took it.
  template <class Event>
  auto process(const Event& event) -> bool {
    return machine_.process_event(event);
  }

  /// @brief Reads the attempt stage.
  ///
  /// @return The stage.
  auto stage() const noexcept -> Stage;
  /// @brief Reports whether an attempt has streamed.
  ///
  /// @return `true` once one streamed.
  auto played() const noexcept -> bool {
    return machine_.is(boost::sml::state<Played>);
  }
  /// @brief Reports when the current attempt first streamed.
  ///
  /// @return The time in monotonic microseconds, or nothing while it has
  ///     not.
  auto attempt_streamed_us() const noexcept -> std::optional<std::uint64_t> {
    return context_.attempt_streamed_us;
  }
  /// @brief Reports when the failures in a row that found this machine
  /// without network began.
  ///
  /// @return The time in monotonic microseconds, or nothing while the last
  ///     failure reached the network.
  auto offline_since_us() const noexcept -> std::optional<std::uint64_t> {
    return context_.offline_since_us;
  }
  /// @brief Gives the stream state to report while no attempt's link runs.
  ///
  /// @return Establishing the route while retiring or resting; the parked
  ///     link's last state once parked; stopped when halted; failed when an
  ///     attempt could not be opened; idle otherwise.
  auto idle_state() const noexcept -> net::protocol::remote_play::StreamState;

 private:
  /// @brief What the machine's actions write.
  Context context_;
  /// @brief The Boost.SML machine over the context.
  boost::sml::sm<Machine> machine_;
};

}  // namespace libchiaking::play::reconnection

namespace libchiaking::play {

/// @brief A console link that retries: each attempt opens a fresh link, and a
/// failed attempt that StartPolicy deems worth it is retired, the console
/// woken when the policy says, and the next opened after a rest.
///
/// Events of a running attempt pass through to the session; while an
/// attempt that has not streamed fails, its failure events are held and
/// dropped if another attempt follows. The last attempt's ending becomes this
/// link's. Input calls go to the current attempt and are refused between
/// attempts. Used on the session thread through the link driver; only
/// ReconnectingLink::wake may come from another thread, guarded by a mutex.
class ReconnectingLink final : public Link, private LinkEventSink {
 public:
  /// @brief Builds a fresh link for one attempt.
  using Opener =
      std::function<Expected<std::unique_ptr<Link>,
                             net::protocol::remote_play::StreamError>()>;

  /// @brief What the link asks of the session holding it.
  class Watcher {
   public:
    /// @brief Allows destruction through the interface.
    virtual ~Watcher() = default;
    /// @brief Wakes the console before a retry; blocks while the request is
    /// sent.
    ///
    /// @return `true` when it was woken; `false` gives up.
    virtual auto wake_console() -> bool = 0;
    /// @brief Reports whether the player is leaving, so failures are not
    /// retried.
    ///
    /// @return `true` while leaving.
    virtual auto leaving() const noexcept -> bool = 0;
    /// @brief Told another attempt is about to start.
    ///
    /// @param attempt The coming attempt, from 1.
    /// @param woke Whether the console was woken first.
    /// @param reconnecting Whether an attempt had streamed before.
    /// @param failure Why the previous attempt ended.
    /// @param rest How long the link rests first.
    virtual void trying_again(std::uint32_t attempt, bool woke,
                              bool reconnecting,
                              const StartPolicy::Failure& failure,
                              std::chrono::milliseconds rest) = 0;
  };

  /// @brief Builds the link; nothing is opened until started.
  ///
  /// @param where Home or internet; decides the attempts allowed.
  /// @param opener Builds each attempt's link.
  /// @param stop Stops the link; checked between attempts.
  /// @param clock The monotonic clock the rest is timed by; must outlive the
  ///     link.
  /// @param threading Provider of the link's mutex and condition variable;
  ///     must outlive the link.
  /// @param watcher The session's side; must outlive the link.
  /// @param handover Whether a home start follows this client's own session
  ///     with the same console.
  /// @param reconnect Whether a drop after streaming is reconnected.
  ReconnectingLink(Where where, Opener opener, platform::StopSource stop,
                   const platform::ClockProvider& clock,
                   platform::ThreadingProvider& threading, Watcher& watcher,
                   ConsoleHandover handover = ConsoleHandover::fresh,
                   bool reconnect = true);
  /// @brief Releases the current attempt.
  ~ReconnectingLink() override;

  /// @brief Opens and starts the first attempt; blocks while it connects.
  ///
  /// @return Nothing, also when a failed first attempt is to be retried; the
  ///     attempt's error when not retried; the opener's error;
  ///     StreamError::invalid_state when already started;
  ///     StreamError::invalid_configuration without an event sink.
  [[nodiscard]] auto start() -> Result override;
  /// @brief Pumps the current attempt, or after the rest opens the next one,
  /// or halts when stopped between attempts.
  ///
  /// @return As the attempt's pump, settled by the policy;
  ///     StreamError::invalid_state when no attempt runs and none follows.
  [[nodiscard]] auto pump_once() -> Result override;
  /// @brief Waits on the current attempt, or for the rest to end or a wake.
  ///
  /// @param timeout The longest wait.
  /// @return The attempt's result, or nothing.
  [[nodiscard]] auto wait(std::chrono::milliseconds timeout) -> Result override;
  /// @brief Wakes the current attempt and any rest; may be called from any
  /// thread.
  void wake() noexcept override;
  /// @brief Passes a datacenter pick to the current attempt.
  ///
  /// @param name The datacenter name.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto select_datacenter(std::string_view name)
      -> Result override;
  /// @brief Stops the current attempt for good and releases it, or halts
  /// between attempts.
  ///
  /// @return The attempt's stop result, or nothing.
  [[nodiscard]] auto stop() -> Result override;
  /// @brief Stops the current attempt for a host sleep and releases it, or
  /// halts between attempts.
  ///
  /// @return The attempt's stop result, or nothing.
  [[nodiscard]] auto stop_for_sleep() -> Result override;
  /// @brief Reads the stream state.
  ///
  /// @return The current attempt's, or the state the machine implies.
  auto state() const noexcept
      -> net::protocol::remote_play::StreamState override;
  /// @brief Sends a controller state to the current attempt.
  ///
  /// @param state The state.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_controller_state(
      const net::protocol::remote_play::ControllerState& state)
      -> Result override;
  /// @brief Sends the login code to the current attempt.
  ///
  /// @param pin The code's bytes.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto submit_pin(std::span<const std::byte> pin)
      -> Result override;
  /// @brief Reports lost video frames to the current attempt; ignored between
  /// attempts.
  ///
  /// @param first The first lost frame index.
  /// @param last The last lost frame index.
  /// @return The attempt's result; nothing between attempts.
  [[nodiscard]] auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Result override;
  /// @brief Runs a command on the current attempt.
  ///
  /// @param command The command.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto execute(
      const net::protocol::remote_play::StreamCommand& command)
      -> Result override;
  /// @brief Sends a Create button press to the current attempt.
  ///
  /// @param press How the button was pressed.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_create(CloudMessaging::CapturePress press)
      -> Result override;
  /// @brief Sends a raw deep link to the current attempt.
  ///
  /// @param text The deep link.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_deep_link(std::string text) -> Result override;
  /// @brief Sends a game invitation to the current attempt.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_invitation(std::string invitation_id,
                                     std::string session_id) -> Result override;
  /// @brief Sends a microphone frame to the current attempt.
  ///
  /// @param frame The encoded frame.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_microphone(std::span<const std::byte> frame)
      -> Result override;
  /// @brief Connects the microphone on the current attempt.
  ///
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto connect_microphone() -> Result override;
  /// @brief Opens or closes the system menu on the current attempt.
  ///
  /// @param pad The pressing controller's pad.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto toggle_system_menu(std::uint32_t pad) -> Result override;
  /// @brief Opens the saved data screen on the current attempt.
  ///
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto open_saved_data() -> Result override;
  /// @brief Mutes or unmutes the microphone on the current attempt.
  ///
  /// @param muted Whether to mute.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto set_microphone_muted(bool muted) -> Result override;
  /// @brief Asks the current attempt for a keyframe.
  ///
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto request_idr() -> Result override;
  /// @brief Discards the current attempt's next video frame.
  ///
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto drop_video_frame() -> Result override;
  /// @brief Sends an audio state message on the current attempt.
  ///
  /// @param kind Which audio state.
  /// @param body Its payload.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_audio_state(
      net::protocol::remote_play::AudioStateKind kind,
      std::span<const std::byte> body) -> Result override;
  /// @brief Sends the keyboard's state on the current attempt.
  ///
  /// @param state The keys down.
  /// @return The attempt's result; StreamError::invalid_state between attempts.
  [[nodiscard]] auto send_keyboard(const KeyboardState& state)
      -> Result override;
  /// @brief Sets where events go; each attempt's events come through this
  /// link.
  ///
  /// @param sink The receiver; must outlive the link.
  void set_event_sink(LinkEventSink& sink) noexcept override;
  /// @brief Reads the diagnostics.
  ///
  /// @return The current attempt's, or the last parked attempt's.
  auto diagnostics() const noexcept
      -> net::protocol::remote_play::StreamDiagnostics override;
  /// @brief Reads the Takion version.
  ///
  /// @return The current attempt's, or the last parked attempt's.
  auto takion_version() const noexcept -> std::optional<std::uint32_t> override;

  /// @brief Collects what the policy needs to know about a link's end.
  ///
  /// @param link The link that ended.
  /// @return Its failure's cause and cancellation, and the console's quit.
  static auto failure_of(const Link& link) -> StartPolicy::Failure;

 private:
  /// @brief Passes an attempt's event on, holding failure events of an attempt
  /// that has not streamed and dropping events while retiring or parking.
  ///
  /// @param event The event.
  void on_event(net::protocol::remote_play::StreamEvent event) override;
  /// @brief Runs a call on the current attempt.
  ///
  /// @param call The call.
  /// @return Its result; StreamError::invalid_state without an attempt.
  auto forwarded(const std::function<Result(LinkInputs&)>& call) -> Result;

  /// @brief Decides what follows an attempt's result: pass it on, give up, or
  /// retire the attempt, wake the console when asked and schedule the next.
  ///
  /// @param result The attempt's result.
  /// @return @p result, or nothing when another attempt follows.
  auto settled(Result result) -> Result;
  /// @brief Reports whether an attempt failed in a way the policy should
  /// judge.
  ///
  /// @param result The attempt's result.
  /// @return `true` for a failed link, a failed start while idle, or a stop
  ///     with an error quit, unless the link was stopped or the player is
  ///     leaving.
  auto attempt_failed(const Result& result) const noexcept -> bool;
  /// @brief Reports whether the link rests between attempts.
  ///
  /// @return `true` while resting.
  auto between_attempts() const noexcept -> bool {
    return progress_.stage() == reconnection::Stage::resting;
  }
  /// @brief Reports whether events pass through now.
  ///
  /// @return `false` while retiring or parking.
  auto passing_events_through() const noexcept -> bool;
  /// @brief Opens and starts the next attempt; blocks while it connects.
  ///
  /// @return The settled start result; the opener's error, after recording a
  ///     resolve failure and reporting the failed state.
  auto open_next() -> Result;
  /// @brief Halts between attempts and reports the stop.
  ///
  /// @return Nothing.
  auto halted() -> Result;
  /// @brief Releases held events, copies the attempt's ending, and releases
  /// the stopped attempt, keeping its diagnostics and version.
  ///
  /// @param stopped The attempt's stop result.
  /// @return @p stopped.
  auto parked(Result stopped) -> Result;
  /// @brief Stops and releases a failed attempt before the next.
  void retire();
  /// @brief Makes the current attempt the last, releasing held events and
  /// copying its ending.
  void conclude();
  /// @brief Copies the current attempt's quit, failure and cleanup failure into
  /// this link's ending, keeping the first of each.
  void mirror();
  /// @brief Passes held events to the sink.
  void release();

  /// @brief The current attempt's link; null between attempts.
  std::unique_ptr<Link> link_;
  /// @brief Builds each attempt's link.
  Opener opener_;
  /// @brief Stops the link.
  platform::StopSource stop_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief The session's side.
  Watcher& watcher_;
  /// @brief Counts attempts and decides retries.
  StartPolicy policy_;
  /// @brief Where events go.
  LinkEventSink* sink_{};
  /// @brief Failure events held from an attempt that has not streamed.
  std::vector<net::protocol::remote_play::StreamEvent> held_;
  /// @brief The reconnection machine.
  reconnection::Progress progress_;
  /// @brief When the rest before the next attempt ends.
  timing::Deadline rest_over_;
  /// @brief The last parked attempt's diagnostics.
  net::protocol::remote_play::StreamDiagnostics last_diagnostics_{};
  /// @brief The last parked attempt's Takion version.
  std::optional<std::uint32_t> last_version_;
  /// @brief Guards the current attempt against a concurrent wake, and the wake
  /// flag.
  platform::Mutex mutex_;
  /// @brief Signalled by a wake during a rest.
  platform::ConditionVariable woken_;
  /// @brief Whether a wake arrived during a rest.
  bool wake_pending_{};
};

}  // namespace libchiaking::play
