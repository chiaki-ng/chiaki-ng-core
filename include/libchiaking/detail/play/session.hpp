/// @file
/// @brief Declares Session, the handle a frontend holds on one running play
/// session.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/detail/play/console_keyboard.hpp"
#include "libchiaking/detail/play/controllers.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"
#include "libchiaking/detail/play/keyboard_state.hpp"
#include "libchiaking/detail/play/login_pin.hpp"
#include "libchiaking/detail/play/microphone.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/preferences.hpp"
#include "libchiaking/detail/play/seats.hpp"
#include "libchiaking/detail/play/statistics.hpp"
#include "libchiaking/detail/users/ps_user.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace libchiaking::diagnostics {
class SessionJournal;
}

namespace libchiaking::play {

class SessionShare;

/// @brief Which kind of connection a session plays over.
enum class Where : std::uint8_t {
  /// @brief Remote play with a console on the local network.
  home,
  /// @brief Remote play with the player's console reached over the internet.
  internet,
  /// @brief A game streamed from the cloud service; there is no console.
  cloud,
};

/// @brief The stage a session is in, as reported by Session::state and
/// SessionEvents::on_state.
///
/// A session normally moves forward through these values. The one step back is
/// from `playing` to `connecting`, which happens when an established stream is
/// lost and the session connects again.
enum class SessionState : std::uint8_t {
  /// @brief The connection is being set up; no stream is running.
  connecting,
  /// @brief The stream is running and input is delivered.
  playing,
  /// @brief The session is shutting down, because the frontend asked or the
  /// stream is stopping.
  leaving,
  /// @brief The session is over and its Outcome is available.
  ended,
};

/// @brief How Session::leave should end the session.
enum class Leave : std::uint8_t {
  /// @brief Close the connection and leave the console as it is.
  disconnect,
  /// @brief Ask the console to enter rest mode first, then close the
  /// connection.
  rest_console,
};

/// @brief One play session, from the first connection attempt until it has
/// ended.
///
/// A Session is created by Play when the frontend starts playing, and is shared
/// through a `std::shared_ptr`. It runs on its own thread, which connects,
/// pumps the stream, and calls the frontend's SessionEvents and MediaSink. The
/// frontend uses this object from its own threads to send input, answer
/// prompts, report how video presentation is going, and leave.
///
/// The parts reached through controllers, seats, login_pin, keyboard,
/// microphone and share belong to the session and stay valid for exactly as
/// long as it does. Requests made through them are queued and carried out by
/// the session's thread; a returned InputRefusal says a request was not queued.
///
/// Destroying the last reference leaves the session if it is still running and
/// waits for its thread to finish.
///
/// @warning Do not destroy a Session from inside one of its own callbacks; the
///     destructor waits for the thread that is making the callback.
///
/// @see Play
/// @see SessionEvents
/// @see MediaSink
/// @ingroup api_play
class Session {
 public:
  /// @brief Implementation behind the session.
  class Core;

  /// @brief How the Create button was pressed, for press_create.
  enum class CreatePress : std::uint8_t {
    /// @brief One short press.
    single_tap,
    /// @brief Two short presses.
    double_tap,
    /// @brief A long press.
    long_press,
  };

  /// @brief What the client shows over or instead of the stream, for
  /// set_system_ui.
  enum class SystemUi : std::uint8_t {
    /// @brief The stream fills the client and nothing covers it.
    game,
    /// @brief The client's own menu covers the stream.
    overlaid,
    /// @brief Nobody can play the stream: the client's screen is off or the
    /// client is put away, which shows the console's home menu.
    background,
  };

  /// @brief Wraps a session implementation.
  ///
  /// Frontends do not construct sessions; Play returns them.
  ///
  /// @param core The implementation; the session takes ownership.
  explicit Session(std::unique_ptr<Core> core) noexcept;
  /// @brief Not copyable; a session has one owner.
  Session(const Session&) = delete;
  /// @brief Not copy-assignable; a session has one owner.
  auto operator=(const Session&) -> Session& = delete;
  /// @brief Leaves the session if it is still running and blocks until its
  /// thread has finished.
  ~Session();

  /// @brief The signed-in player the session plays as.
  ///
  /// @return A reference valid for as long as the session.
  auto player() const noexcept -> const users::PsUser&;
  /// @brief The kind of connection this session plays over; fixed for its
  /// lifetime.
  ///
  /// @return The kind of connection this session plays over; fixed for its
  ///     lifetime.
  auto where() const noexcept -> Where;
  /// @brief The current stage of the session.
  ///
  /// @return A snapshot. It may already be ahead of the last
  ///     SessionEvents::on_state call, because that call is made later by the
  ///     session's thread.
  auto state() const noexcept -> SessionState;
  /// @brief The console this session plays on.
  ///
  /// @return The console, or null when the session has none. The pointer is
  ///     valid for as long as the session.
  auto console() const noexcept -> const consoles::Console*;
  /// @brief The cloud launch that this session is carrying out.
  ///
  /// @return The launch for a cloud session that has one, valid for as long
  ///     as the session; null for home and internet sessions.
  auto launch() noexcept -> CloudLaunch*;
  /// @brief The journal that collects this session's diagnostic log.
  ///
  /// @return A shared pointer that is never null. Keeping a copy keeps the
  ///     journal readable after the session is destroyed.
  auto journal() const noexcept
      -> const std::shared_ptr<diagnostics::SessionJournal>&;

  /// @brief The session's controller input queue.
  ///
  /// @return The session's controller input queue.
  auto controllers() noexcept -> Controllers&;
  /// @brief The session's seats, for adding and removing further local
  /// players.
  ///
  /// @return The session's seats, for adding and removing further local
  ///     players.
  auto seats() noexcept -> Seats&;
  /// @brief The prompt for the console's login PIN.
  ///
  /// @see SessionEvents::on_login_pin_needed
  ///
  /// @return The prompt for the console's login PIN.
  auto login_pin() noexcept -> LoginPin&;
  /// @brief The console's on-screen keyboard.
  ///
  /// @see SessionEvents::on_keyboard_opened
  ///
  /// @return The console's on-screen keyboard.
  auto keyboard() noexcept -> ConsoleKeyboard&;
  /// @brief The session's microphone.
  ///
  /// @return The session's microphone.
  auto microphone() noexcept -> Microphone&;
  /// @brief The control for sharing this session's stream and seats with
  /// guests.
  ///
  /// @return The control for sharing this session's stream and seats with
  ///     guests.
  auto share() noexcept -> SessionShare&;

  /// @brief Asks the console to return to its home screen.
  ///
  /// The request is queued and sent by the session's thread while the stream
  /// is running. Whether the console acted on it is not reported.
  ///
  /// @return Nothing on success.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::wrong_route The session is a cloud session.
  /// @retval InputRefusal::not_ready A rest mode request is under way.
  [[nodiscard]] auto go_home() -> Expected<void, InputRefusal>;
  /// @brief Sends what a keyboard holds down to the console.
  ///
  /// The console takes it as a USB keyboard: it types into the console's
  /// text fields and reaches games that read a keyboard. Each state replaces
  /// the last one, so send one whenever a key or lock changes, and an empty
  /// state to let go of every key. The state is queued and sent by the
  /// session's own thread; a send failure is written to the session journal
  /// rather than returned. Safe to call from any thread.
  ///
  /// @param state The keys, modifiers and locks to report.
  /// @return Nothing on success; the state is queued.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto send_keyboard(const KeyboardState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Sends a Create button press to a PS5 cloud service, so the cloud
  /// console takes a screenshot or clip.
  ///
  /// The press travels as a cloud gateway message to the capture address,
  /// which the service must have listed in its address handshake first. The
  /// request is queued and sent by the session's thread; whether the service
  /// accepted it is written to the session journal.
  ///
  /// @param press How the button was pressed.
  /// @return Nothing on success.
  /// @retval InputRefusal::wrong_route The session is not a PS5 cloud session.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto press_create(CreatePress press)
      -> Expected<void, InputRefusal>;
  /// @brief Opens a LegacyCloudStream service's Application Saved Data
  /// Management screen in the stream, so saved data can move between PS Plus
  /// cloud storage and streaming
  /// storage.
  ///
  /// The service is sent the typed SAVE_DATA deep link. The request is queued
  /// and sent by the session's thread. Safe to call from any thread.
  ///
  /// @return Nothing when queued.
  /// @retval InputRefusal::wrong_route The session is not a LegacyCloudStream
  /// session.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto open_saved_data() -> Expected<void, InputRefusal>;
  /// @brief Has a PS5 cloud or LegacyCloudStream service join a game
  /// invitation.
  ///
  /// The service is sent the typed INVITE deep link with the invitation's id
  /// and its game session's id, both read from the invitation's PSN
  /// notification (consoles::GameInvitation). The request is queued and sent
  /// by the session's thread; the service's reply is written to the session
  /// journal. Safe to call from any thread.
  ///
  /// @param invitation_id The invitation's id.
  /// @param session_id The invited game session's id.
  /// @return Nothing when queued.
  /// @retval InputRefusal::wrong_route The session is not a cloud session.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto join_invitation(std::string invitation_id,
                                     std::string session_id)
      -> Expected<void, InputRefusal>;
  /// @brief Tells a PS5 cloud service whether the app's own menu covers the
  /// stream or nobody can play it, such as while a quick menu is open or the
  /// screen is off.
  ///
  /// The service is sent the `psaction:systemservicestatus` deep links for
  /// `isSystemUiOverlaid` and `isInBackgroundExecution`: SystemUi::overlaid
  /// sets the first, SystemUi::background the second and SystemUi::game
  /// clears both. The state is remembered, also before the session is
  /// running, and sent by the session's thread once the stream runs. Setting
  /// the state it already has does nothing. Safe to call from any thread.
  ///
  /// @param state What the app shows over or instead of the stream.
  /// @return Nothing on success.
  /// @retval InputRefusal::wrong_route The session is not a PS5 cloud session.
  [[nodiscard]] auto set_system_ui(SystemUi state)
      -> Expected<void, InputRefusal>;
  /// @brief Hands changed accessibility settings to a running PS5 cloud
  /// session.
  ///
  /// The launch request already carries the settings the session started
  /// with; this sends the `psaction:accessibility` deep link with the screen
  /// reader, its speed and volume, large text, bold text and higher contrast,
  /// so the cloud console can apply them without restarting the stream. The
  /// deep link describes text size as the PS5 setting does, on or off, so
  /// large and very large text both turn it on. Settings equal to the last
  /// ones queued are dropped, unless sending those failed; the first call is
  /// always queued, so callers should skip settings equal to those the
  /// session launched with. The request is remembered, also before the
  /// session is running, and sent by the session's thread once the stream
  /// runs. Safe to call from any thread.
  ///
  /// @param accessibility The settings the player now wants.
  /// @return Nothing on success.
  /// @retval InputRefusal::wrong_route The session is not a PS5 cloud session.
  [[nodiscard]] auto set_accessibility(
      const AccessibilityRequest& accessibility)
      -> Expected<void, InputRefusal>;
  /// @brief Turns 3D audio for this session on or off.
  ///
  /// A PS5 console or PS5 cloud server renders the stereo stream binaurally
  /// when asked. A console session selects the console's 3D stereo audio
  /// profile; a cloud session sets the cloud speaker flag. LegacyCloudStream
  /// has no such mode and refuses it. A PS4's protocol has no audio profiles,
  /// so the choice is kept but nothing is sent.
  ///
  /// The choice is remembered, also before the session is running. It is sent
  /// when the stream's audio is ready, again whenever the audio format
  /// changes, and at once when changed while playing. Setting the value it
  /// already has does nothing.
  ///
  /// @param enabled True to ask for 3D audio.
  /// @return Nothing on success.
  /// @retval InputRefusal::wrong_route The session is a LegacyCloudStream
  /// session.
  [[nodiscard]] auto set_audio_3d(bool enabled) -> Expected<void, InputRefusal>;
  /// @brief The 3D audio choice last made with set_audio_3d; false until
  /// then, and always false on a session that refuses 3D audio.
  ///
  /// @return The 3D audio choice last made with set_audio_3d; false until
  ///     then, and always false on a session that refuses 3D audio.
  auto audio_3d() const noexcept -> bool;

  /// @brief Tells the session that the frontend has shown a video frame.
  ///
  /// Call this each time a decoded frame reaches the screen. It also resets
  /// the count of decoder rejections that would otherwise lead to a keyframe
  /// request. Takes no lock, so it may be called for every frame from the
  /// frontend's decoder or render thread.
  ///
  /// @return The loss accumulated since the previous call, which the frontend
  ///     can use to mark the shown frame as following a gap; empty when the
  ///     session is no longer SessionState::connecting or
  ///     SessionState::playing, so the frame should not be shown.
  auto video_presented() noexcept -> std::optional<media::PresentationLoss>;
  /// @brief Tells the session that the decoder rejected a frame after
  /// MediaSink::on_video had already returned.
  ///
  /// The session reports the lost range to the console and, after repeated
  /// rejections with nothing presented, asks for a keyframe. A rejection
  /// known at the time on_video returns is reported with
  /// VideoVerdict::recover instead.
  ///
  /// @param frame The rejected frame's VideoFrame::frame_index.
  /// @param cause The decoder's own error code; recorded in the statistics
  ///     and the journal.
  void video_rejected(std::uint16_t frame, int cause);
  /// @brief Tells the session that video output has failed and cannot
  /// continue.
  ///
  /// This records a media fault of kind MediaFailure::decoder_output and the
  /// session ends with it.
  ///
  /// @param cause The frontend's own error code; kept in the fault.
  void video_output_failed(int cause);
  /// @brief Asks the console to send a keyframe.
  ///
  /// Use it when the decoder has lost its reference frames for a reason the
  /// session cannot see, for example after the decoder was rebuilt.
  ///
  /// @return True when the request was noted; false when the session is not
  ///     running.
  auto request_keyframe() -> bool;
  /// @brief Discards the next video frame as if the network had lost it.
  ///
  /// A testing aid: the stream then recovers exactly as it would from a real
  /// loss, reporting the frame and repointing later frames at a surviving
  /// reference.
  ///
  /// @return True when the request was noted; false when the session is not
  ///     running.
  auto drop_video_frame() -> bool;

  /// @brief Starts leaving the session.
  ///
  /// The call returns at once; the session ends later, which
  /// SessionEvents::on_ended reports and wait_until_ended waits for. With
  /// Leave::rest_console the console is asked to enter rest mode and the
  /// connection is kept open briefly so the request can go out.
  ///
  /// @param how Whether to rest the console before closing.
  /// @return True when leaving was started as asked. False when the session
  ///     had already ended, and also when Leave::rest_console was asked for
  ///     but the rest request could not be made, for example on a cloud
  ///     session; in that second case the session is still left, by
  ///     disconnecting.
  auto leave(Leave how = Leave::disconnect) -> bool;
  /// @brief Leaves the session because the host computer is about to sleep.
  ///
  /// The session is stopped without the usual cleanup with the account
  /// service, so that it finishes before the host suspends.
  ///
  /// @param rest_console True to also ask the console to enter rest mode
  ///     first; when that request cannot be made the session is stopped all
  ///     the same.
  void leave_for_host_sleep(bool rest_console) noexcept;

  /// @brief Returns a snapshot of the session's link, video, audio,
  /// microphone and input statistics.
  ///
  /// @return A snapshot of the session's link, video, audio, microphone and
  ///     input statistics.
  auto statistics() const -> Statistics;
  /// @brief The result of the session, once it has ended.
  ///
  /// @return Nothing while the session is still running.
  auto outcome() const -> std::optional<Outcome>;
  /// @brief Blocks until the session has ended and SessionEvents::on_ended
  /// has returned.
  ///
  /// Called from inside on_ended it returns at once.
  ///
  /// @return The session's outcome; the reference is valid for as long as the
  ///     session.
  /// @warning Calling this from any other session callback never returns,
  ///     because the session cannot end while its thread is inside that
  ///     callback.
  auto wait_until_ended() -> const Outcome&;

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play
