/// @file
/// @brief Declares SessionEvents, the callbacks through which a session reports
/// its progress, prompts and end.
#pragma once

#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/detail/play/cloud_notice.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/seats.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/progress.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::play {

/// @brief The step a connection attempt has reached.
///
/// Which steps occur depends on the route: pairing only when the console is not
/// yet registered, the account session and route steps only over the internet.
enum class ConnectStep : std::uint8_t {
  /// @brief Looking for the console's address.
  finding_console,
  /// @brief Opening the account session that brokers a connection over the
  /// internet.
  opening_account_session,
  /// @brief Waiting for the console to wake from rest mode.
  waking_console,
  /// @brief Registering this device with the console.
  pairing,
  /// @brief Opening a path to the console for the control connection.
  reaching_console,
  /// @brief Opening the path that will carry the stream.
  opening_stream_route,
  /// @brief Asking the console to start a remote play session.
  requesting_session,
  /// @brief Establishing the control connection.
  opening_control,
  /// @brief Measuring the connection before the stream starts.
  measuring_connection,
  /// @brief Starting the stream and waiting for its description.
  starting_stream,
};

/// @brief Why the previous connection attempt failed, when a session is trying
/// again.
struct AttemptFailure {
  /// @brief The failure's cause, when one was identified.
  std::optional<EndReason::Cause> cause{};
  /// @brief The reason the console gave for closing the session, when it gave
  /// one.
  std::optional<ConsoleQuit::Reason> console_quit{};
  /// @brief Whether the attempt failed because this machine had no network,
  /// so it never reached PSN or the console, and the session is still waiting
  /// for the network: it keeps trying for a while without using up its
  /// attempts. False once that wait is over, when failures count again.
  bool offline{};

  /// @brief Compares every field.
  friend constexpr auto operator==(const AttemptFailure&,
                                   const AttemptFailure&) noexcept
      -> bool = default;
};

/// @brief One report of how connecting is going, passed to
/// SessionEvents::on_connect_progress.
struct ConnectProgress {
  /// @brief The step this report is about.
  ConnectStep step{ConnectStep::finding_console};
  /// @brief Time the attempt has taken so far, as measured when the step was
  /// reported; zero in a report that announces a wait before the next
  /// attempt.
  std::chrono::microseconds elapsed{};
  /// @brief Number of the current attempt, counting from 1.
  std::uint32_t attempt{1};
  /// @brief True once an established stream has been lost and the session is
  /// connecting again.
  bool reconnecting{};
  /// @brief Present when the session is waiting rather than working: for a
  /// retry, for the console to wake, or for the console to join.
  std::optional<progress::Wait> wait{};
  /// @brief Why the attempt before this one failed; absent on the first
  /// attempt.
  std::optional<AttemptFailure> previous_failure{};
  /// @brief Detail of the internet route being opened; absent on other
  /// routes.
  std::optional<progress::InternetRoute> route{};

  /// @brief Converts the report to the generic progress::Status shared with
  /// other long-running work.
  ///
  /// @return A status carrying the attempt, elapsed time and wait, with no
  ///     position.
  constexpr auto status() const noexcept -> progress::Status {
    return {std::nullopt, attempt, elapsed, wait};
  }

  /// @brief Compares every field.
  friend constexpr auto operator==(const ConnectProgress&,
                                   const ConnectProgress&) noexcept
      -> bool = default;
};

/// @brief Why a cloud session is going to end on its own.
enum class PlayTimeLimit : std::uint8_t {
  /// @brief The session is reaching the longest time the service allows.
  session_limit,
  /// @brief The streaming server is about to go down for maintenance.
  maintenance,
  /// @brief The service gave a reason this library does not know.
  other,
};

/// @brief Receives everything a session reports to the frontend.
///
/// The frontend implements this interface and passes it to Play when starting a
/// session. The object must stay alive until the Session has been destroyed.
///
/// Every callback is made on the session's own thread, never on the caller's. A
/// callback should return quickly, because the stream is not serviced while it
/// runs. It may call back into the Session to queue input or to leave, but must
/// not destroy it and must not call Session::wait_until_ended, except from
/// on_ended.
///
/// Only on_state and on_ended must be implemented; the others do nothing by
/// default.
///
/// @see Session
/// @see MediaSink
class SessionEvents {
 public:
  /// @brief Destroys the receiver.
  virtual ~SessionEvents() = default;

  /// @brief Called each time the session enters a new stage.
  ///
  /// The first call reports SessionState::connecting. Calls arrive in the
  /// order the stages were entered; SessionState::ended is reported once,
  /// just before on_ended.
  ///
  /// @param state The stage entered.
  virtual void on_state(SessionState state) = 0;
  /// @brief Called once, last, when the session is over.
  ///
  /// No callback of this interface is made after it.
  ///
  /// @param outcome Why and how the session ended. The reference is valid for
  ///     as long as the Session; Session::outcome returns the same value.
  virtual void on_ended(const Outcome& outcome) = 0;

  /// @brief Called as connecting advances, when an attempt is about to be
  /// retried, and while reconnecting.
  ///
  /// The parameter is the latest ConnectProgress; it is valid only during the
  /// call.
  virtual void on_connect_progress(const ConnectProgress&) {}
  /// @brief Called as a cloud launch advances.
  ///
  /// The parameter is the latest LaunchProgress; it is valid only during the
  /// call. Only cloud sessions report it.
  ///
  /// @see CloudLaunch
  virtual void on_launch_progress(const LaunchProgress&) {}
  /// @brief Called when a cloud session has measured the data centres it can
  /// play from.
  ///
  /// The parameter is the DatacenterOffer listing them; it is valid only
  /// during the call.
  virtual void on_datacenters_offered(const DatacenterOffer&) {}
  /// @brief Called when the data centre for a cloud session has been settled.
  ///
  /// The parameter is the DatacenterChoice; it is valid only during the call.
  virtual void on_datacenter_chosen(const DatacenterChoice&) {}

  /// @brief Called when the console asks for its login PIN before it lets the
  /// session in.
  ///
  /// The parameter is true when the PIN entered before was wrong and the
  /// console is asking again. Answer through Session::login_pin.
  ///
  /// @see LoginPin
  virtual void on_login_pin_needed(bool) {}
  /// @brief Called when the console starts or stops withholding the picture.
  ///
  /// The parameter is true when the console prohibits showing its screen over
  /// remote play, and false when the picture is allowed again.
  virtual void on_picture_blocked(bool) {}
  /// @brief Called when the stream starts or stops receiving nothing for five
  /// seconds while it plays.
  ///
  /// The parameter is true while nothing arrives, as when the network drops,
  /// and false once the stream receives again. The session keeps waiting up
  /// to its silence limit, then ends or, at home and over the internet,
  /// reconnects as Preferences::reconnect_after_drop allows.
  virtual void on_connection_waiting(bool) {}
  /// @brief Called when a cloud service warns that the session will close.
  ///
  /// The first parameter is the time left before the service closes the
  /// game, and the second why it will. Unsaved progress is lost when it
  /// does. Services repeat the warning as the time runs down; console
  /// sessions never call this.
  virtual void on_play_time_left(std::chrono::minutes, PlayTimeLimit) {}
  /// @brief Called when a PS5 cloud service sends a system notification,
  /// such as a saved capture or an earned trophy.
  ///
  /// The parameter is the notice; it is valid only during the call. Console
  /// sessions never call this.
  ///
  /// @see CloudNotice
  virtual void on_cloud_notice(const CloudNotice&) {}
  /// @brief Called when a PS5 cloud service starts, updates or ends the
  /// banner it keeps up while recording a clip.
  ///
  /// The parameter is the banner's state; it is valid only during the call.
  /// Updates arrive about once a second with the time recorded so far.
  ///
  /// @see RecordingBanner
  virtual void on_recording_banner(const RecordingBanner&) {}
  /// @brief Called when a PS5 cloud service has sent the thumbnail of a
  /// screenshot or clip it saved.
  ///
  /// The parameter is the capture; it is valid only during the call. It
  /// usually follows the CloudNotice that says the capture was saved.
  ///
  /// @see CloudCapture
  virtual void on_capture_thumbnail(const CloudCapture&) {}
  /// @brief Called when a PS5 cloud service has finished uploading a capture
  /// to the account's cloud media gallery.
  ///
  /// The parameter names the capture in the gallery; it is valid only during
  /// the call.
  ///
  /// @see CloudCaptureUpload
  virtual void on_capture_uploaded(const CloudCaptureUpload&) {}

  /// @brief Called when the console opens its on-screen keyboard.
  ///
  /// The parameter is the text already in the keyboard; it is valid only
  /// during the call. Answer through Session::keyboard.
  ///
  /// @see ConsoleKeyboard
  virtual void on_keyboard_opened(std::string_view) {}
  /// @brief Called when the console reports that the text in its keyboard
  /// changed.
  ///
  /// The parameter is the new text; it is valid only during the call.
  virtual void on_keyboard_text(std::string_view) {}
  /// @brief Called when the console closes its keyboard itself.
  ///
  /// ConsoleKeyboard::active is false from here on.
  virtual void on_keyboard_closed() {}

  /// @brief Called when a PS5 console wants the passcode of the account
  /// joining a seat.
  ///
  /// The parameters are the seat, and whether this is a repeat request after
  /// a passcode was entered. Answer with Seats::enter_passcode. The seat
  /// reads as SeatState::entering_passcode.
  virtual void on_seat_passcode_needed(controllers::SeatNumber, bool) {}
  /// @brief Called when the console announces that an added seat's controller
  /// has joined.
  ///
  /// The parameters are the seat and the colour the console gave its light
  /// bar. The seat reads as SeatState::playing from here on. A PS4 console
  /// answers Seats::add_player this way.
  ///
  /// @see SeatConfirmed
  virtual void on_seat_joined(controllers::SeatNumber, controllers::SeatLight) {
  }
  /// @brief Called when an occupied added seat becomes empty without the
  /// frontend asking.
  ///
  /// This happens when the console drops the seat, and for every occupied
  /// added seat when the stream is lost and the session starts connecting
  /// again. The parameter is the seat, which reads as SeatState::empty.
  virtual void on_seat_left(controllers::SeatNumber) {}
  /// @brief Called when a request to fill a seat failed.
  ///
  /// The parameter says which seat and why; see SeatRefusal. The seat reads
  /// as SeatState::empty again.
  virtual void on_seat_refused(const SeatRefusal&) {}
};

}  // namespace libchiaking::play
