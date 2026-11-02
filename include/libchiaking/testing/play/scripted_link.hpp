/// @file
/// @brief Declares ScriptedLink, a scripted stand-in for the connection to a
/// console or cloud server.
#pragma once

#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/detail/play/controllers.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/seats.hpp"
#include "libchiaking/detail/play/session_events.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play::testing {

/// @brief Plays the far end of a play session from a script.
///
/// A Session normally talks to a console or a cloud server through a link that
/// sets up a route, negotiates a stream and carries media and commands. This
/// double replaces that link. Start a session with Play::start_scripted and the
/// session runs for real, with its own thread, events, seats, controllers and
/// media path, while the test decides what the "console" does and sees what the
/// session sent to it.
///
/// The members fall in three groups. The first scripts refusals and stepping
/// and is called before the session starts. The second, from progress() to
/// finish(), makes the console do something: connect(), then describe() and
/// show() for a picture, join() for a seat, fail() or quit() to end. The third
/// reads back what the session sent: calls(), count(), controller_samples(),
/// seat_requests(), keyframe_requests() and login_pin().
///
/// Console actions are not performed by the calling thread. Each is queued and
/// performed in order by the session's own thread, so the matching
/// SessionEvents callback arrives a little later, on that thread; a test waits
/// for the event rather than expecting it on return. Actions queued before the
/// session starts are performed once it is running. An action called from
/// inside a then() step is performed at once.
///
/// A short script: construct the link, start the session with
/// Play::start_scripted, call connect() and wait for SessionState::playing,
/// drive the session's public API, assert on count() or seat_requests(), then
/// call finish() and wait for the session to end.
///
/// It differs from a real link in that nothing is sent anywhere, every command
/// from the session succeeds unless a refusal is scripted, and nothing happens
/// unless the script says so: the link never connects, times out or ends by
/// itself.
///
/// @note The link must outlive the session started on it. Its members may be
///     called from any thread.
class ScriptedLink {
 public:
  /// @brief Implementation state, defined inside the library.
  class Core;

  /// @brief A piece of test code to run on the session's thread; see then().
  ///
  /// It receives the link it was queued on.
  using Step = std::function<void(ScriptedLink&)>;

  /// @brief One seat command the session sent to the console.
  ///
  /// @see seat_requests
  struct SeatRequest {
    /// @brief Which seat command was sent.
    enum class Kind : std::uint8_t {
      /// @brief The session named the account a seat should sign in as.
      choose_account,
      /// @brief The session asked for a seat to join.
      join,
      /// @brief The session asked for a seat to be removed.
      remove,
      /// @brief The session answered a passcode prompt for a seat.
      passcode
    };

    /// @brief Which command this is.
    Kind kind{Kind::join};
    /// @brief The seat the command named.
    controllers::SeatNumber seat{};
    /// @brief The account named; present only for Kind::choose_account.
    std::optional<AccountId> account{};
    /// @brief The controller kind requested; present only for Kind::join.
    std::optional<controllers::ControllerKind> controller{};

    /// @brief Compares every field, so a test can assert on a whole
    /// request.
    friend constexpr auto operator==(const SeatRequest&,
                                     const SeatRequest&) noexcept
        -> bool = default;
  };

  /// @brief The fixed sample stream that start_shared_stream() and
  /// send_shared_media() play.
  ///
  /// A test compares what an audience received against these values.
  ///
  /// @see shared_media
  struct SharedMedia {
    /// @brief The stream protocol version the sample stream is announced
    /// with.
    std::uint32_t protocol_version{};
    /// @brief The frame rate of the sample picture.
    std::uint32_t frames_per_second{};
    /// @brief The stream description start_shared_stream() announces: one
    /// H.264 picture profile and stereo audio.
    media::StreamDescription description{};
    /// @brief The video frames, in order, that send_shared_media()
    /// delivers.
    std::vector<std::vector<std::byte>> video_frames{};
    /// @brief The audio frames, in order, that send_shared_media()
    /// delivers.
    std::vector<std::vector<std::byte>> audio_frames{};
  };

  /// @brief Returns the sample stream used for Kakusan tests.
  ///
  /// @return The same values on every call.
  static auto shared_media() -> SharedMedia;

  /// @brief Creates a link with an empty script and nothing recorded.
  ///
  /// @param threading Provider of the script's mutex and condition variable
  ///     and of the crypto that seals shared media; must outlive the link.
  /// @param clock Monotonic clock the script's waits measure on; must
  ///     outlive the link.
  ScriptedLink(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock);
  /// @brief Not copyable; a running session refers to this link.
  ScriptedLink(const ScriptedLink&) = delete;
  /// @brief Not assignable; a running session refers to this link.
  auto operator=(const ScriptedLink&) -> ScriptedLink& = delete;
  /// @brief Destroys the link.
  ///
  /// @pre No session started on it is still running.
  ~ScriptedLink();

  /// @brief Makes the link fail as soon as the session tries to start it.
  ///
  /// Plays a console that cannot be reached. Call it before
  /// Play::start_scripted.
  ///
  /// @param reason The reason the link gives for failing to start.
  void refuse_start(EndReason reason);
  /// @brief Makes the link reject requests to seat a player.
  ///
  /// Affects the commands that name a seat's account and that ask a seat to
  /// join; removals and passcodes still succeed. Rejected requests are still
  /// recorded in seat_requests().
  ///
  /// @param cause The cause the rejection carries.
  void refuse_seat_requests(EndReason::Cause cause);
  /// @brief Makes the link reject a submitted login PIN.
  ///
  /// The PIN is still recorded and can be read with login_pin().
  ///
  /// @param cause The cause the rejection carries.
  void refuse_login_pin(EndReason::Cause cause);
  /// @brief Sets the stream protocol version the link says it negotiated.
  ///
  /// Until this or start_shared_stream() is called the link reports no
  /// version.
  ///
  /// @param version The version to report to the session.
  void report_protocol_version(std::uint32_t version);
  /// @brief Queues test code to run on the session's thread, in script order.
  ///
  /// Use it to do something at an exact point between two console actions, or
  /// to call the session from the thread its callbacks arrive on. Console
  /// actions called from inside the step are performed immediately instead of
  /// being queued.
  ///
  /// @param step The code to run. It is called once with this link.
  void then(Step step);
  /// @brief Makes the session's thread wait for turn() between rounds of
  /// work.
  ///
  /// Without this the session's thread runs freely. With it, the thread parks
  /// each time it would idle and performs queued actions only when the test
  /// calls turn(), which makes the order of events exact. Call it before
  /// starting the session. Stepping stops by itself once the session is asked
  /// to leave, so that it can shut down.
  void step_by_hand();
  /// @brief Lets the session's thread do one round of work.
  ///
  /// Waits for the thread to park, releases it, and waits for it to park
  /// again.
  ///
  /// @return True when the thread is parked again and another turn can be
  ///     given. False when step_by_hand() is not in force, or the link has
  ///     stopped or failed.
  auto turn() -> bool;

  /// @brief Reports that connecting has reached a step.
  ///
  /// @param step The step to report.
  /// @see SessionEvents::on_connect_progress
  void progress(ConnectStep step);
  /// @brief Reports progress in finding a route over the internet.
  ///
  /// @param step The route step to report, with its purpose.
  /// @see SessionEvents::on_connect_progress
  void route(progress::InternetRoute step);
  /// @brief Reports progress in launching a cloud title.
  ///
  /// Delivered only when the session was started with Where::cloud; otherwise
  /// the call has no effect.
  ///
  /// @param progress The progress to report.
  /// @see SessionEvents::on_launch_progress
  void launch_progress(LaunchProgress progress);
  /// @brief Reports the datacenters a cloud launch can choose from.
  ///
  /// Delivered only when the session was started with Where::cloud; otherwise
  /// the call has no effect.
  ///
  /// @param offer The datacenters on offer.
  /// @see SessionEvents::on_datacenters_offered
  void offer_datacenters(DatacenterOffer offer);
  /// @brief Reports which datacenter a cloud launch settled on.
  ///
  /// Delivered only when the session was started with Where::cloud; otherwise
  /// the call has no effect.
  ///
  /// @param choice The datacenter chosen.
  /// @see SessionEvents::on_datacenter_chosen
  void choose_datacenter(DatacenterChoice choice);
  /// @brief Makes the link report that the stream is up.
  ///
  /// This is what takes a started session to SessionState::playing.
  void connect();
  /// @brief Announces the stream's audio format and picture profiles.
  ///
  /// A real console sends this once the stream is negotiated, before any
  /// media.
  ///
  /// @param description The audio format, video profiles and, when set, the
  ///     picture the console granted.
  void describe(media::StreamDescription description);
  /// @brief Delivers one encoded video frame from the console.
  ///
  /// @param frame The frame; its bytes and its metadata are passed on as
  ///     given.
  /// @see MediaSink
  void show(media::VideoFrame frame);
  /// @brief Reports that a video frame was lost beyond recovery.
  void lose_video();
  /// @brief Delivers one audio unit from the console.
  ///
  /// @param unit Either a format change or encoded audio, as its kind says.
  /// @see MediaSink
  void sound(media::AudioUnit unit);
  /// @brief Delivers controller feedback, such as rumble, from the console.
  ///
  /// @param feedback The feedback to deliver.
  void feed_back(controllers::ControllerFeedback feedback);
  /// @brief Prepares the session's share to forward the sample stream.
  ///
  /// Gives the share fresh stream keys, sets the reported protocol version to
  /// that of shared_media(), and announces the sample stream's description as
  /// describe() would. Records `shared_stream` in calls(), or
  /// `shared_stream_untapped` when no session has been started on the link.
  ///
  /// @see send_shared_media
  void start_shared_stream();
  /// @brief Plays the sample stream's packets into the session's share.
  ///
  /// The packets carry the frames listed in shared_media() and are sealed
  /// with the keys start_shared_stream() issued, so an audience that has
  /// joined the share receives exactly those frames. Records `shared_media`
  /// in calls(), or `shared_media_unsealed` when the packets could not be
  /// prepared, for instance because start_shared_stream() was not called
  /// first.
  void send_shared_media();
  /// @brief Makes the console ask for its login PIN.
  ///
  /// @param previous_was_wrong True when asking again after a wrong PIN.
  /// @see SessionEvents::on_login_pin_needed
  void ask_for_login_pin(bool previous_was_wrong);
  /// @brief Reports that the console has blocked or unblocked the picture.
  ///
  /// @param blocked True when the console forbids showing the current scene.
  /// @see SessionEvents::on_picture_blocked
  void block_picture(bool blocked);
  /// @brief Makes the console open its on-screen keyboard.
  ///
  /// @param text The text already in the field.
  /// @see SessionEvents::on_keyboard_opened
  void open_keyboard(std::string_view text);
  /// @brief Reports that the console changed the keyboard's text.
  ///
  /// @param text The new text.
  /// @see SessionEvents::on_keyboard_text
  void change_keyboard_text(std::string_view text);
  /// @brief Makes the console close its on-screen keyboard.
  ///
  /// @see SessionEvents::on_keyboard_closed
  void close_keyboard();
  /// @brief Makes the console ask for a seat's account passcode.
  ///
  /// @param seat The seat being signed in.
  /// @param retry True when asking again after a wrong passcode.
  /// @see SessionEvents::on_seat_passcode_needed
  void ask_for_passcode(controllers::SeatNumber seat, bool retry);
  /// @brief Makes the console announce that a seat has joined.
  ///
  /// @param seat The seat that joined.
  /// @param light The light colour the console assigned to it.
  /// @see SessionEvents::on_seat_joined
  void join(controllers::SeatNumber seat, controllers::SeatLight light);
  /// @brief Makes the console announce that a seat has left.
  ///
  /// @param seat The seat that left.
  /// @see SessionEvents::on_seat_left
  void drop(controllers::SeatNumber seat);
  /// @brief Makes the console refuse a seat.
  ///
  /// Only refusals a console can send are played: one saying the account is
  /// not on the console, or one carrying a console status code. A refusal
  /// that names no seat, or one that describes a failure on this side of the
  /// link, is ignored without notice.
  ///
  /// @param refusal The refusal to play.
  /// @see SessionEvents::on_seat_refused
  void refuse(SeatRefusal refusal);
  /// @brief Makes the link report that it has begun shutting the stream down.
  ///
  /// The link keeps running until finish(), fail() or quit() is played.
  void begin_leaving();
  /// @brief Makes the link report that it has failed.
  ///
  /// The link stops running and hands the session the reason.
  ///
  /// @param reason The reason the link gives for failing.
  /// @see SessionEvents::on_ended
  void fail(EndReason reason);
  /// @brief Records that tidying up after the session failed.
  ///
  /// This does not end the link. It only records the reason as the link's
  /// clean-up failure.
  ///
  /// @param reason The reason clean-up failed.
  void fail_cleanup(EndReason reason);
  /// @brief Makes the console end the session with a reason of its own.
  ///
  /// The link records the reason and text and then stops as finish() does.
  ///
  /// @param quit The reason and text the console gives.
  /// @see SessionEvents::on_ended
  void quit(ConsoleQuit quit);
  /// @brief Makes the link stop cleanly, as when a stream ends normally.
  ///
  /// Has no effect once the link has already stopped or failed.
  ///
  /// @see SessionEvents::on_ended
  void finish();

  /// @brief Lists what the session did to the link, in order.
  ///
  /// Each entry is a fixed name. Life cycle: `start`, `stop`,
  /// `stop_for_sleep`, `wake_console`. Input and media: `controller`,
  /// `keyframe`, `video_loss`, `microphone_connect`, `microphone_frame`,
  /// `microphone_mute`, `microphone_unmute`, `audio_state`, `keyboard`.
  /// Console commands:
  /// `login_pin`, `rest`, `go_home`, `keyboard_text`, `keyboard_accept`,
  /// `keyboard_reject`, `seat_account`, `seat_join`, `seat_remove`,
  /// `seat_passcode`, and `command` for any other. The shared stream helpers
  /// add the names given in their own descriptions.
  ///
  /// @return A snapshot of the names recorded so far.
  auto calls() const -> std::vector<std::string>;
  /// @brief Counts how often one name appears in calls().
  ///
  /// @param call The name to count, spelled as calls() lists it.
  /// @return The number of times it has been recorded so far.
  auto count(std::string_view call) const -> std::size_t;
  /// @brief Returns the controller states the session sent for a seat.
  ///
  /// @param seat The seat whose controller to read.
  /// @return Every state sent for that seat, oldest first.
  auto controller_samples(controllers::SeatNumber seat) const
      -> std::vector<controllers::ControllerState>;
  /// @brief Returns the seat commands the session sent, oldest first.
  ///
  /// Requests the link was scripted to refuse are included.
  ///
  /// @return A copy of every seat command received so far.
  auto seat_requests() const -> std::vector<SeatRequest>;
  /// @brief Reports how many times the session asked for a fresh keyframe.
  ///
  /// @return The count since the link was created.
  auto keyframe_requests() const -> std::size_t;
  /// @brief Returns the login PIN the session last submitted.
  ///
  /// @return A wiping copy of the PIN's characters, or an empty secret when
  ///     none was submitted.
  auto login_pin() const -> SecretString;

  /// @brief Gives the library access to the link's implementation.
  ///
  /// Used by Play::start_scripted. Tests have no use for it.
  ///
  /// @return The implementation, owned by this link.
  auto core() noexcept -> Core& { return *core_; }

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play::testing
