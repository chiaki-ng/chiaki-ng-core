/// @file
/// @brief Declares Session::Core, the running session behind the public
/// Session: its thread, its loop over the link, and every input, prompt and
/// media path the frontend reaches through it.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/play/session_events.hpp"
#include "libchiaking/detail/play/statistics.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/media/video_intake.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/datacenters/datacenter_selector.hpp"
#include "src/core/play/adapters/links/cloud/cloud_messaging.hpp"
#include "src/core/play/adapters/links/link.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/media/media_delivery.hpp"
#include "src/core/play/adapters/media/media_units.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "src/core/play/adapters/session/console_commands.hpp"
#include "src/core/play/adapters/session/controllers_core.hpp"
#include "src/core/play/adapters/session/prompts/console_keyboard_core.hpp"
#include "src/core/play/adapters/session/prompts/login_pin_core.hpp"
#include "src/core/play/adapters/session/prompts/microphone_core.hpp"
#include "src/core/play/adapters/session/rest_request.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"
#include "src/core/play/adapters/session/session_settings.hpp"
#include "src/core/play/adapters/session/session_share_core.hpp"
#include "src/core/play/adapters/session/locked_session_lifecycle.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::play {

/// @brief One play session from opening to its Outcome.
///
/// Owns the link driver, the command route, the controller and seat queues,
/// the console commands, the media path and the public faces built over
/// them. Session::Core::begin starts a thread that starts the link, runs the
/// loop (media faults, loss reports and keyframes, commands, pumping the link,
/// waiting) until the stream ends, the player leaves or the console rests,
/// then stops the link and settles the Outcome, telling the SessionEvents on
/// the way. Frontend threads call the input and leave members at any time;
/// each part locks its own state, and the core's own state is under one
/// mutex. The destructor cancels and joins the thread, so it must not run on
/// the session thread. Not copyable.
class Session::Core final : public DatacenterSelector, private LinkDriverSink {
 public:
  /// @brief Everything a session opens with. Plain data built by the start
  /// use case.
  struct Opening {
    /// @brief The user playing; required.
    std::shared_ptr<users::PsUser> player;
    /// @brief The console played on; null for the cloud.
    std::shared_ptr<const consoles::Console> console;
    /// @brief Where the session runs.
    Where where{Where::home};
    /// @brief The cloud service; present exactly for a cloud session.
    std::optional<catalog::ServiceGeneration> service;
    /// @brief The link the session drives; required.
    std::unique_ptr<Link> link;
    /// @brief Stops the link.
    platform::StopSource stop;
    /// @brief Receives video and audio; must outlive the session.
    media::MediaSink& media;
    /// @brief Receives the session's events; must outlive the session.
    SessionEvents& events;
    /// @brief The monotonic clock; must outlive the session.
    platform::ClockProvider& clock;
    /// @brief Makes the session's thread, mutexes and condition variables;
    /// must outlive the session.
    platform::ThreadingProvider& threading;
    /// @brief The journal to write to; a fresh one, numbered 1, when null.
    std::shared_ptr<diagnostics::SessionJournal> journal;
    /// @brief The kind of the player's own controller.
    controllers::ControllerKind first_controller{
        controllers::ControllerKind::console_default};
    /// @brief Timing, tolerances and limits of the loop.
    SessionSettings settings{core_settings()};
    /// @brief Lets other players join the session; a closed one when null.
    std::shared_ptr<SessionShare::Core> share{};
    /// @brief When a controller state wakes the session thread; only changed
    /// samples when the network's socket calls are costly.
    Controllers::Core::Waking controller_waking{
        Controllers::Core::Waking::every_sample};
  };

  /// @brief What the link reports that only the start's holder can act on:
  /// cloud launch progress and datacenters, the console's name and the launch
  /// refusal. Every member has an empty default. Called on the session thread.
  class LinkReports {
   public:
    /// @brief Allows destruction through the interface.
    virtual ~LinkReports() = default;
    /// @brief Told the cloud launch progressed; ignored by default.
    virtual void launch_progress(
        const net::protocol::remote_play::event::SetupProgress&) {}
    /// @brief Told the cloud datacenters were measured; ignored by default.
    virtual void datacenters_measured(
        const net::protocol::remote_play::event::CloudDatacenters&) {}
    /// @brief Told the cloud datacenter was selected; ignored by default.
    virtual void datacenter_selected(
        const net::protocol::remote_play::event::CloudDatacenterSelected&) {}
    /// @brief Told the console's name; ignored by default.
    virtual void console_named(std::string_view) {}
    /// @brief Asks why the cloud launch was refused.
    ///
    /// @return Empty by default.
    virtual auto launch_refused() -> std::optional<LaunchRefused> {
      return std::nullopt;
    }
  };

  /// @brief Why Session::Core::begin did not start the session.
  enum class NotBegun : std::uint8_t {
    /// @brief The session was already begun or run.
    already_begun,
    /// @brief The host could not start the session's thread.
    no_thread,
  };

  /// @brief Opens a session without starting it.
  ///
  /// @param opening What the session opens with.
  /// @return The core; EndReason::Cause::invalid_preferences without a player
  ///     or link, or when the cloud service is given for a console session or
  ///     missing for a cloud one.
  [[nodiscard]] static auto open(Opening opening)
      -> Expected<std::unique_ptr<Core>, EndReason::Cause>;

  /// @brief Cancels the session, joins its thread, closes sharing and keeps the
  /// journal. Blocks until the thread ends; must not run on the session thread.
  ~Core() override;
  /// @brief Not copyable; the session owns its thread.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; the session owns its thread.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Attaches the receiver of link reports; only before the session
  /// begins.
  ///
  /// @param reports The receiver; must outlive the session.
  /// @return `true` when attached; `false` once begun.
  auto attach(LinkReports& reports) -> bool;
  /// @brief Attaches the public face of a cloud launch; only before the
  /// session begins.
  ///
  /// @param launch The face; must outlive the session.
  /// @return `true` when attached; `false` once begun or for a console session.
  auto attach(CloudLaunch& launch) -> bool;
  /// @brief Starts the session on its own thread.
  ///
  /// @return Nothing once the thread is running.
  /// @retval NotBegun::already_begun The session was already begun or run.
  /// @retval NotBegun::no_thread The host could not start the thread; the
  ///     session never runs and may only be destroyed.
  [[nodiscard]] auto begin() -> Expected<void, NotBegun>;
  /// @brief Runs the session on the calling thread instead, blocking until it
  /// ends.
  ///
  /// @return `true` after running; `false` when it was already begun.
  auto run_on_this_thread() -> bool;
  /// @brief Reports that the link is about to try again; called on the session
  /// thread by the retrying link.
  ///
  /// A lost stream sends the session back to connecting and empties the extra
  /// seats.
  ///
  /// @param attempt The coming attempt, from 1.
  /// @param waking Whether the console is being woken first.
  /// @param reconnecting Whether a stream had started before.
  /// @param failure Why the previous attempt ended.
  /// @param rest How long the link rests first.
  void starting_again(std::uint32_t attempt, bool waking, bool reconnecting,
                      AttemptFailure failure, std::chrono::milliseconds rest);

  /// @brief Reads the player.
  ///
  /// @return The user playing.
  auto player() const noexcept -> const users::PsUser& { return *player_; }
  /// @brief Reads where the session runs.
  ///
  /// @return Home, internet or cloud.
  auto where() const noexcept -> Where { return where_; }
  /// @brief Reads the cloud service.
  ///
  /// @return The service for a cloud session; empty otherwise.
  auto cloud_service() const noexcept
      -> std::optional<catalog::ServiceGeneration> {
    return service_;
  }
  /// @brief Reads the console.
  ///
  /// @return The console; null for the cloud.
  auto console() const noexcept -> const consoles::Console* {
    return console_.get();
  }
  /// @brief Reads the session's state. Takes no lock.
  ///
  /// @return The state last published.
  auto state() const noexcept -> SessionState;
  /// @brief Gives the cloud launch's face.
  ///
  /// @return The face; null for a console session.
  auto launch() noexcept -> CloudLaunch* { return launch_; }
  /// @brief Gives the public sharing face.
  ///
  /// @return The face, owned by the share.
  auto share() noexcept -> SessionShare* { return &share_->face(); }
  /// @brief Gives the share's rules.
  ///
  /// @return The share.
  auto share_rules() noexcept -> SessionShare::Core& { return *share_; }
  /// @brief Gives the session's events.
  ///
  /// @return The frontend's events.
  auto events() noexcept -> SessionEvents& { return events_; }
  /// @brief Gives the console commands.
  ///
  /// @return The commands, owned by the core.
  auto commands() noexcept -> ConsoleCommands& { return commands_; }
  /// @brief Gives the lifecycle.
  ///
  /// @return The lifecycle, owned by the core.
  auto lifecycle() const noexcept -> const LockedSessionLifecycle& {
    return lifecycle_;
  }
  /// @brief Reads the link's state.
  ///
  /// @return The stream state.
  auto link_state() const noexcept -> net::protocol::remote_play::StreamState {
    return driver_.state();
  }
  /// @brief Gives the journal.
  ///
  /// @return The journal the session writes to.
  auto journal() const noexcept
      -> const std::shared_ptr<diagnostics::SessionJournal>& {
    return journal_;
  }
  /// @brief Reports whether the caller is the session thread.
  ///
  /// @return `true` on the thread running the session.
  auto on_core_thread() const noexcept -> bool;

  /// @brief Gives the public seats.
  ///
  /// @return The seats, owned by the core.
  auto seats() noexcept -> Seats& { return seats_; }
  /// @brief Gives the seat rules.
  ///
  /// @return The rules, owned by the core.
  auto seat_rules() noexcept -> Seats::Core& { return seat_rules_; }
  /// @brief Gives the public controllers.
  ///
  /// @return The controllers, owned by the core.
  auto controllers() noexcept -> Controllers& { return controllers_; }
  /// @brief Gives the controller rules.
  ///
  /// @return The rules, owned by the core.
  auto controller_rules() noexcept -> Controllers::Core& {
    return controller_rules_;
  }
  /// @brief Gives the public login prompt.
  ///
  /// @return The prompt, owned by the core.
  auto login_pin() noexcept -> LoginPin& { return login_pin_; }
  /// @brief Gives the public keyboard prompt.
  ///
  /// @return The prompt, owned by the core.
  auto keyboard() noexcept -> ConsoleKeyboard& { return keyboard_; }
  /// @brief Gives the public microphone.
  ///
  /// @return The microphone, owned by the core.
  auto microphone() noexcept -> Microphone& { return microphone_; }

  /// @brief Queues showing the console's home screen.
  ///
  /// @return As ConsoleCommands::go_home.
  [[nodiscard]] auto go_home() -> Expected<void, InputRefusal>;
  /// @brief Switches 3D audio.
  ///
  /// @param enabled Whether 3D audio is wanted.
  /// @return As ConsoleCommands::set_audio_3d.
  auto set_audio_3d(bool enabled) -> Expected<void, InputRefusal>;
  /// @brief Queues a Create button press for a PS5 cloud service.
  ///
  /// @param press How the button was pressed.
  /// @return As ConsoleCommands::press_create.
  auto press_create(Session::CreatePress press) -> Expected<void, InputRefusal>;
  /// @brief Opens a LegacyCloudStream service's Application Saved Data
  /// Management screen.
  ///
  /// @return As ConsoleCommands::open_saved_data.
  auto open_saved_data() -> Expected<void, InputRefusal>;
  /// @brief Has a PS5 cloud or LegacyCloudStream service join a game
  /// invitation.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return As ConsoleCommands::join_invitation.
  auto join_invitation(std::string invitation_id, std::string session_id)
      -> Expected<void, InputRefusal>;
  /// @brief Tells a PS5 cloud service whether the client's UI covers the
  /// stream or the stream cannot be seen.
  ///
  /// @param state What the client shows over or instead of the stream.
  /// @return As ConsoleCommands::set_system_ui.
  auto set_system_ui(Session::SystemUi state) -> Expected<void, InputRefusal>;
  /// @brief Hands changed accessibility settings to a PS5 cloud service.
  ///
  /// @param accessibility The settings the player now wants.
  /// @return As ConsoleCommands::set_accessibility.
  auto set_accessibility(const AccessibilityRequest& accessibility)
      -> Expected<void, InputRefusal>;
  /// @brief Sends the keyboard's state.
  ///
  /// @param state The keys down.
  /// @return As ConsoleCommands::send_keyboard.
  auto send_keyboard(const KeyboardState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Reads the 3D audio setting.
  ///
  /// @return Whether 3D audio is wanted.
  auto audio_3d() const noexcept -> bool;
  /// @brief Records that the frontend presented a frame. Takes no lock.
  ///
  /// @return The losses since the last presentation; empty when the state is
  ///     neither SessionState::connecting nor SessionState::playing.
  auto video_presented() noexcept -> std::optional<media::PresentationLoss>;
  /// @brief Records that the decoder rejected a frame and wakes the session
  /// thread to report it.
  ///
  /// @param frame The frame index.
  /// @param cause The decoder's cause.
  void video_rejected(std::uint16_t frame, int cause);
  /// @brief Records that the decoder failed while presenting, which ends the
  /// session, and wakes the session thread.
  ///
  /// @param cause The decoder's cause.
  void video_output_failed(int cause);
  /// @brief Asks for a keyframe and wakes the session thread.
  ///
  /// @return `true` while the stream runs; `false` otherwise.
  auto request_keyframe() -> bool;
  /// @brief Asks for the next video frame to be discarded and wakes the
  /// session thread.
  ///
  /// @return `true` while the stream runs; `false` otherwise.
  auto drop_video_frame() -> bool;
  /// @brief Passes the player's datacenter pick to the link.
  ///
  /// @param name The datacenter name.
  /// @return Nothing; StreamError::invalid_state once the session is ending or
  ///     ended; otherwise the link's answer.
  auto select_datacenter(std::string_view name)
      -> Expected<void, net::protocol::remote_play::StreamError> override;
  /// @brief Asks the session to end: interrupts waits and cancels the link
  /// unless the session is parked for sleep.
  void cancel() noexcept;
  /// @brief Parks a running session for a host sleep, or cancels one that is
  /// not running.
  void stop_for_host_sleep() noexcept;
  /// @brief Ends the session with a rest-mode request, or another way when the
  /// request cannot be made.
  ///
  /// @param reason Why.
  /// @return `true` when the request was queued; `false` after parking for
  ///     sleep or cancelling instead.
  auto stop_with_rest(RestRequest::Reason reason) -> bool;
  /// @brief Wakes the session thread.
  void wake() noexcept;
  /// @brief Ends the session the way the player asked.
  ///
  /// @param how Whether to rest the console too.
  /// @return `false` once ended; for a rest, as Session::Core::stop_with_rest;
  ///     otherwise `true`.
  auto leave(Leave how) -> bool;
  /// @brief Ends the session because the host is going to sleep.
  ///
  /// @param rest_console Whether to rest the console too.
  void leave_for_host_sleep(bool rest_console) noexcept;
  /// @brief Collects the running statistics.
  ///
  /// @return Link, protocol, loop, video, audio, microphone and controller
  ///     figures.
  auto statistics() const -> Statistics;
  /// @brief Reads the outcome.
  ///
  /// @return The outcome once settled; empty before.
  auto outcome() const -> std::optional<Outcome>;
  /// @brief Blocks until the session ended and its end was delivered.
  ///
  /// On the session thread it returns as soon as the outcome is settled.
  ///
  /// @return The outcome, owned by the core.
  auto wait_until_ended() -> const Outcome&;

  /// @brief Gives the library's loop settings.
  ///
  /// @return The defaults with the public controller and seat depths.
  static auto core_settings() -> SessionSettings;
  /// @brief Names the decoder stages after the sink's decoder.
  ///
  /// @param media The frontend's sink.
  /// @param settings The settings to start from.
  /// @return The settings with decoder stage names that start with the
  ///     decoder's name.
  static auto named_for(const media::MediaSink& media, SessionSettings settings)
      -> SessionSettings;
  /// @brief Maps a link stage to the connect step the player sees.
  ///
  /// @param stage The link stage.
  /// @return The step; empty for a stage the player does not see.
  static auto connect_step(
      net::protocol::remote_play::FailureStage stage) noexcept
      -> std::optional<ConnectStep>;
  /// @brief Reports whether a link stage belongs to a cloud launch.
  ///
  /// @param stage The link stage.
  /// @return `true` for the cloud provisioning stages.
  static auto is_launch_stage(
      net::protocol::remote_play::FailureStage stage) noexcept -> bool;

 private:
  /// @brief Builds the core and binds its share.
  ///
  /// @param opening What the session opens with; its parts are moved out.
  /// @param journal The journal to write to.
  Core(Opening& opening, std::shared_ptr<diagnostics::SessionJournal> journal);

  /// @brief How the loop ended.
  struct LoopEnd {
    /// @brief Where it ended; none when it ended normally.
    Outcome::Phase phase{Outcome::Phase::none};
    /// @brief The link's error, when one ended it.
    std::optional<net::protocol::remote_play::StreamError> error;
  };

  /// @brief Routes a link event: media to the media path, the stream
  /// description to the decoder, everything else to the commands and the
  /// reports. Called on the session thread.
  ///
  /// @param event The event.
  void on_event(net::protocol::remote_play::StreamEvent event) override;
  /// @brief Passes a video frame to the media path; called on the session
  /// thread.
  ///
  /// @param frame The frame.
  void on_video_frame(media::VideoFrame frame) override;

  /// @brief Reports whether the link is connecting or streaming.
  ///
  /// @param state The link's state.
  /// @return `true` while establishing the route, negotiating or streaming.
  static auto active(net::protocol::remote_play::StreamState state) noexcept
      -> bool;
  /// @brief Converts the link's diagnostics into public statistics.
  ///
  /// @param wired The link's diagnostics.
  /// @return The statistics.
  static auto link_statistics(
      const net::protocol::remote_play::StreamDiagnostics& wired)
      -> LinkStatistics;
  /// @brief Describes a stream for the log.
  ///
  /// @param info The stream description.
  /// @return The video profiles and the audio format.
  static auto describe(
      const net::protocol::remote_play::event::StreamInfoReady& info)
      -> std::string;

  /// @brief Marks the session begun.
  ///
  /// @return `true` the first time.
  auto claim() -> bool;
  /// @brief Runs the session loop until the stream ends.
  ///
  /// @return How it ended.
  auto loop() -> LoopEnd;
  /// @brief Stops the link and settles the outcome from what is known.
  ///
  /// A session that ended because the console went to rest drops the failures
  /// that resting caused.
  ///
  /// @param end How the loop ended.
  /// @return The outcome.
  auto finish(const LoopEnd& end) -> Outcome;
  /// @brief Counts a failed pump, and copies the link's diagnostics and
  /// Takion version for statistics once the link's state changed or
  /// SessionSettings::statistics_interval passed since the last copy.
  ///
  /// @param pump_failed Whether the last pump failed, counted as a loop error.
  void snapshot(bool pump_failed);
  /// @brief Copies the link's diagnostics and Takion version for statistics
  /// now, and times the next copy from now.
  ///
  /// @param state The link's state, remembered to notice its next change.
  /// @param now The current instant.
  void copy_statistics(net::protocol::remote_play::StreamState state,
                       timing::Deadline::Instant now);
  /// @brief Writes one line to the journal.
  ///
  /// @param severity The line's severity.
  /// @param message The line.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

  /// @brief Moves the session to playing or leaving as the stream does.
  ///
  /// @param changed The link's state change.
  void report(const net::protocol::remote_play::event::StateChanged& changed);
  /// @brief Tells the frontend the console wants a login code.
  ///
  /// @param required The request.
  void report(const net::protocol::remote_play::event::PinRequired& required);
  /// @brief Passes launch progress to the reports, or connect progress to the
  /// frontend.
  ///
  /// @param progress The link's progress.
  void report(const net::protocol::remote_play::event::SetupProgress& progress);
  /// @brief Tells the frontend whether the console blocks its picture.
  ///
  /// @param display The notice.
  void report(
      const net::protocol::remote_play::event::DisplayProhibited& display);
  /// @brief Tells the frontend a cloud service will close the game.
  ///
  /// @param left The warning.
  void report(const net::protocol::remote_play::event::PlayTimeLeft& left);
  /// @brief Tells the frontend whether nothing has arrived for a while.
  ///
  /// @param waiting The notice.
  void report(
      const net::protocol::remote_play::event::ConnectionWaiting& waiting);
  /// @brief Tells the frontend about a cloud service's system notification,
  /// recording banner, capture thumbnail or finished capture upload.
  ///
  /// @param message The addressed message.
  void report(const net::protocol::remote_play::event::CloudMessage& message);
  /// @brief Tells the frontend the console opened its keyboard.
  ///
  /// @param keyboard The notice with the starting text.
  void report(const net::protocol::remote_play::event::KeyboardOpen& keyboard);
  /// @brief Tells the frontend the keyboard's text changed.
  ///
  /// @param keyboard The notice with the text.
  void report(
      const net::protocol::remote_play::event::KeyboardTextChange& keyboard);
  /// @brief Tells the frontend the console closed its keyboard.
  ///
  /// @param keyboard The notice.
  void report(
      const net::protocol::remote_play::event::KeyboardRemoteClose& keyboard);
  /// @brief Tells the frontend a seat needs its passcode.
  ///
  /// @param request The request.
  void report(
      const net::protocol::remote_play::event::PadPasscodeRequest& request);
  /// @brief Empties a seat the console refused and tells the frontend and the
  /// share.
  ///
  /// @param failed The refusal.
  void report(const net::protocol::remote_play::event::PadJoinFailed& failed);
  /// @brief Marks a seat joined and tells the frontend, with its light colour,
  /// and the share.
  ///
  /// @param announced The announcement.
  void report(const net::protocol::remote_play::event::PadAnnounced& announced);
  /// @brief Empties a seat the console dropped and tells the frontend and the
  /// share.
  ///
  /// @param dropped The notice.
  void report(const net::protocol::remote_play::event::PadDropped& dropped);
  /// @brief Empties a seat whose account the console could not resolve and
  /// tells the frontend and the share.
  ///
  /// @param unresolvable The notice.
  void report(const net::protocol::remote_play::event::PadAccountUnresolvable&
                  unresolvable);
  /// @brief Passes measured datacenters to the reports.
  ///
  /// @param datacenters The event.
  void report(
      const net::protocol::remote_play::event::CloudDatacenters& datacenters);
  /// @brief Passes the selected datacenter to the reports.
  ///
  /// @param selected The event.
  void report(const net::protocol::remote_play::event::CloudDatacenterSelected&
                  selected);
  /// @brief Passes the console's name to the reports.
  ///
  /// @param named The event.
  void report(const net::protocol::remote_play::event::NicknameReceived& named);

  /// @brief The session thread's body: starts the link, runs the loop,
  /// settles the outcome and tells the frontend it ended.
  void run();
  /// @brief Moves the session forward to a later state; an earlier or equal one
  /// is ignored.
  ///
  /// @param next The state.
  void advance(SessionState next);
  /// @brief Sends a playing session back to connecting after the stream was
  /// lost and empties the extra seats.
  void link_lost();
  /// @brief Tells the frontend every state change not yet told, in order.
  void publish();

  /// @brief Starts the session's thread.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock statistics copies are timed on.
  const platform::ClockProvider& clock_;
  /// @brief The user playing.
  std::shared_ptr<users::PsUser> player_;
  /// @brief The console; null for the cloud.
  std::shared_ptr<const consoles::Console> console_;
  /// @brief Where the session runs.
  Where where_;
  /// @brief The cloud service; empty for consoles.
  std::optional<catalog::ServiceGeneration> service_;
  /// @brief The frontend's events.
  SessionEvents& events_;
  /// @brief The journal written to.
  std::shared_ptr<diagnostics::SessionJournal> journal_;
  /// @brief Lets other players join.
  std::shared_ptr<SessionShare::Core> share_;
  /// @brief Timing, tolerances and limits.
  SessionSettings settings_;
  /// @brief Where the session stands and why it will end.
  LockedSessionLifecycle lifecycle_;
  /// @brief Drives the link.
  LinkDriver driver_;
  /// @brief Where commands go.
  CommandRoute route_;
  /// @brief Collects a cloud console's capture thumbnails and uploads.
  CloudCaptures captures_;
  /// @brief The controller queues.
  Controllers::Core controller_rules_;
  /// @brief The seats.
  Seats::Core seat_rules_;
  /// @brief The console commands.
  ConsoleCommands commands_;
  /// @brief The media path.
  MediaDelivery media_;
  /// @brief The public seats.
  Seats seats_;
  /// @brief The public controllers.
  Controllers controllers_;
  /// @brief The public login prompt.
  LoginPin login_pin_;
  /// @brief The public keyboard prompt.
  ConsoleKeyboard keyboard_;
  /// @brief The public microphone.
  Microphone microphone_;
  /// @brief The link reports' receiver; null when none is attached.
  LinkReports* reports_{};
  /// @brief The cloud launch's face; null for consoles.
  CloudLaunch* launch_{};
  /// @brief Guards changes to state_, and guards unpublished_, outcome_,
  /// begun_, delivered_, link_, takion_version_, pump_errors_ and runner_, and
  /// the attaching of reports_ and launch_ and the start of thread_, which all
  /// happen before the session begins.
  ///
  /// attempt_, previous_failure_ and reconnecting_ are not guarded by it:
  /// they belong to the session thread alone, which writes and reads them
  /// only while it holds the link's turn (LinkDriver::Turn).
  mutable platform::Mutex mutex_;
  /// @brief Signalled when the end is delivered.
  platform::ConditionVariable changed_;
  /// @brief The session's state; changed only with mutex_ held, read without
  /// it.
  std::atomic<SessionState> state_{SessionState::connecting};
  /// @brief State changes not yet told to the frontend.
  std::deque<SessionState> unpublished_{SessionState::connecting};
  /// @brief The outcome, once settled.
  std::optional<Outcome> outcome_;
  /// @brief The current attempt, from 1; set by the retrying link. Session
  /// thread only, inside the link's turn; not guarded by mutex_.
  std::uint32_t attempt_{1};
  /// @brief Why the previous attempt ended. Session thread only, inside the
  /// link's turn; not guarded by mutex_.
  std::optional<AttemptFailure> previous_failure_;
  /// @brief Whether the stream was lost and is being reconnected. Session
  /// thread only, inside the link's turn; not guarded by mutex_.
  bool reconnecting_{};
  /// @brief Whether the session was begun.
  bool begun_{};
  /// @brief Whether the end was told to the frontend.
  bool delivered_{};
  /// @brief The link's last diagnostics.
  net::protocol::remote_play::StreamDiagnostics link_{};
  /// @brief The negotiated Takion version.
  std::optional<std::uint32_t> takion_version_;
  /// @brief Pump failures counted by Session::Core::snapshot.
  std::uint64_t pump_errors_{};
  /// @brief When the link's diagnostics are next copied for statistics.
  /// Session thread only; not guarded by mutex_.
  timing::Deadline statistics_due_{timing::Deadline::Instant{}};
  /// @brief The link's state at the last copy of its diagnostics. Session
  /// thread only; not guarded by mutex_.
  net::protocol::remote_play::StreamState statistics_state_{
      net::protocol::remote_play::StreamState::idle};
  /// @brief The thread running the session.
  platform::ThreadIdentity runner_{};
  /// @brief The session's own thread, when begun with Session::Core::begin.
  platform::Thread thread_;
};

}  // namespace libchiaking::play
