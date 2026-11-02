/// @file
/// @brief Declares usecases::Host, the use case of hosting a shared session:
/// the host's commands, the play::Session's callbacks and the delivery of
/// events to the frontend.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/shared_session.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::kakusan {

class Surroundings;

namespace usecases {

/// @brief Carries out a shared session for the host.
///
/// Commands from the frontend take the hosting's lock, tell the protocol core,
/// translate what the core produced into queued steps, and wake the session.
/// The play::Session calls back as the shared stream's audience: keys and
/// description, encrypted datagrams, rumble, seat changes, and service() on
/// every turn of its loop. service() runs a pump turn through the PumpGate and
/// delivers the queued steps with no lock held, so events reach the observer
/// on the session's thread and never from inside a command. Steps that touch
/// the console (seating, dropping a seat, input, keyframes) are delivered the
/// same way.
///
/// Owned by SharedSession::Hosting. Commands may be called from any thread;
/// the audience callbacks come from the session, encrypted_datagram() possibly
/// from its receive path. Neither copyable nor movable.
class Host final : private play::SharedAudience {
 public:
  /// @brief Step: ask the console to seat a guest.
  struct Joining {
    /// @brief The guest.
    GuestId guest;
    /// @brief Their Kakusan seat.
    SeatGrant grant;
  };
  /// @brief Step: give up the console seat held for a guest.
  struct Removal {
    /// @brief The guest.
    GuestId guest;
  };
  /// @brief Step: send a guest's controller state to their console seat.
  struct Input {
    /// @brief The guest.
    GuestId guest;
    /// @brief The state for their console seat.
    controllers::ControllerState state;
  };
  /// @brief Step: ask the console for a keyframe.
  struct Keyframe {};
  /// @brief Step: write a diagnostic line to the log.
  struct Note {
    /// @brief The text, without the "Kakusan" prefix.
    std::string text;
  };

  /// @brief Anything delivered after a pump turn: an event for the observer
  /// or work for the console.
  using Step = std::variant<SharedSession::Event, Joining, Removal, Input,
                            Keyframe, Note>;

  /// @brief Binds the use case to its hosting.
  /// @param hosting The hosting that owns this object.
  /// @param surroundings The Kakusan context.
  /// @param machine The protocol core.
  /// @param observer Receives the events; must outlive the hosting.
  /// @param pacing Timing and limits.
  Host(SharedSession::Hosting& hosting, Surroundings& surroundings,
       SharedSession::Core& machine, SharedSession::Observer& observer,
       SharedSession::Pacing pacing) noexcept;
  /// @brief Not copyable.
  Host(const Host&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Host&) -> Host& = delete;

  /// @brief Starts sharing a session, as Kakusan::share does.
  ///
  /// Checks the session can be shared, opens the transport source (resolving
  /// STUN servers, which blocks), builds the core and the hosting, and begins
  /// the share with the session. Each refusal is logged.
  ///
  /// @param surroundings The Kakusan context.
  /// @param session The session to share; must outlive the result.
  /// @param rules Seats and allowed buttons.
  /// @param observer Receives the events; must outlive the result.
  /// @param pacing Timing and limits.
  /// @return The shared session.
  /// @retval Refusal::Kind::cannot_be_shared A cloud session, or a protocol
  ///     version that cannot be forwarded.
  /// @retval Refusal::Kind::busy The session is already shared.
  /// @retval Refusal::Kind::not_sharing The session has ended.
  /// @retval Refusal The core could not be built from the rules or no session
  ///     id could be drawn.
  [[nodiscard]] static auto open(std::shared_ptr<Surroundings> surroundings,
                                 play::SessionShare& session,
                                 const SharingRules& rules,
                                 SharedSession::Observer& observer,
                                 SharedSession::Pacing pacing)
      -> Expected<std::unique_ptr<SharedSession>, Refusal>;

  /// @brief Starts an invitation; see SharedSession::invite_player.
  /// @return The new guest's id.
  /// @retval Refusal::Kind::not_sharing Sharing stopped, or the stream is not
  ///     yet shared.
  /// @retval Refusal Any failure of SharedSession::Core::invite.
  [[nodiscard]] auto invite_player() -> Expected<GuestId, Refusal>;
  /// @brief Accepts a guest's reply; see SharedSession::accept_reply.
  /// @param pasted The reply text.
  /// @return The guest's id.
  /// @retval Refusal::Kind::invalid_argument The text is empty or longer than
  ///     Invitation::most_text.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  /// @retval Refusal Any failure of SharedSession::Core::import_reply.
  [[nodiscard]] auto accept_reply(std::string_view pasted)
      -> Expected<GuestId, Refusal>;
  /// @brief Enters a guest's code; see SharedSession::verify.
  /// @param guest The guest.
  /// @param theirs Their code.
  /// @return Nothing once taken.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  /// @retval Refusal Any failure of SharedSession::Core::enter_code.
  [[nodiscard]] auto verify(const GuestId& guest,
                            const VerificationCode& theirs)
      -> Expected<void, Refusal>;
  /// @brief Approves a guest for a seat; see SharedSession::give_seat.
  /// @param guest The guest.
  /// @param seating How the console should seat them.
  /// @return Nothing once recorded.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  /// @retval Refusal::Kind::internal The guest is unknown.
  [[nodiscard]] auto give_seat(const GuestId& guest,
                               const GuestSeating& seating)
      -> Expected<void, Refusal>;
  /// @brief Removes a guest; see SharedSession::take_back_seat.
  /// @param guest The guest.
  /// @return Nothing once the removal has begun.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  /// @retval Refusal::Kind::internal The guest is unknown.
  [[nodiscard]] auto take_back_seat(const GuestId& guest)
      -> Expected<void, Refusal>;
  /// @brief Changes the seat counts; see SharedSession::change.
  ///
  /// Applies capacity and local seats in the order that keeps the table valid
  /// in between: capacity first when growing, local seats first when
  /// shrinking.
  ///
  /// @param rules The new counts; the allowed buttons must not change.
  /// @return Nothing once in force.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  /// @retval Refusal::Kind::invalid_argument The allowed buttons differ.
  /// @retval Refusal Any failure of the seat table.
  [[nodiscard]] auto change(const SharingRules& rules)
      -> Expected<void, Refusal>;
  /// @brief Changes the media tuning and logs the values asked for; see
  /// SharedSession::tune.
  /// @param tuning The new tuning, converted to microseconds.
  /// @return Nothing on success.
  /// @retval Refusal::Kind::not_sharing Sharing stopped.
  [[nodiscard]] auto tune(const Tuning& tuning) -> Expected<void, Refusal>;
  /// @brief Stops sharing; see SharedSession::stop_sharing.
  ///
  /// Stops the share and gives up the console seats outside the lock, which
  /// blocks until the session has no audience call in progress, then tells
  /// the core the stream ended. Does nothing the second time.
  void stop_sharing();

 private:
  /// @brief The refusal for a reason the session cannot be shared.
  /// @param reason The session's reason.
  /// @return Refusal::Kind::cannot_be_shared, not_sharing or busy.
  static constexpr auto unshareable(play::NotShareable reason) noexcept
      -> Refusal {
    switch (reason) {
      case play::NotShareable::cloud_session:
      case play::NotShareable::protocol_version:
        return Refusal::Kind::cannot_be_shared;
      case play::NotShareable::session_ended:
        return Refusal::Kind::not_sharing;
      case play::NotShareable::already_shared:
        break;
    }
    return Refusal::Kind::busy;
  }
  /// @brief Converts a tuning duration to microseconds, negative as zero.
  /// @param duration The duration.
  /// @return The duration in microseconds.
  static constexpr auto microseconds(
      std::chrono::milliseconds duration) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::microseconds{
        std::max(duration, std::chrono::milliseconds::zero())}
                                          .count());
  }
  /// @brief Formats a duration for the tuning log line.
  /// @param duration The duration.
  /// @return Its count of milliseconds as text.
  static auto milliseconds(std::chrono::milliseconds duration) -> std::string;

  /// @brief Takes the stream's keys and description; reports StreamShared, or
  /// SharingFailed when the stream cannot be forwarded (too many profiles, an
  /// oversized header, or a setup the core refuses).
  /// @param stream The stream.
  /// @param keys Its receive keys.
  void stream_started(const play::SharedStream& stream,
                      play::ReceiveKeys keys) override;
  /// @brief Hands one encrypted datagram to the core's capture and notes the
  /// pump has work. Takes no lock and never blocks beyond the capture's.
  /// @param datagram The datagram.
  void encrypted_datagram(
      const play::SharedDatagram& datagram) noexcept override;
  /// @brief Keeps the haptic intensity setting, or passes a rumble, scaled by
  /// it, to the guest in its seat.
  /// @param feedback The console's feedback.
  void feedback(const controllers::ControllerFeedback& feedback) override;
  /// @brief Tells the core the console seated a guest, and reports
  /// PlayerSeated; drops the console seat when the core refuses.
  /// @param seat The console seat.
  void guest_seated(controllers::SeatNumber seat) override;
  /// @brief Drops the console seat request and tells the core the join
  /// failed.
  /// @param refusal Which seat and why.
  void guest_seat_refused(const play::SeatRefusal& refusal) override;
  /// @brief Tells the core a guest lost their console seat and reports
  /// PlayerUnseated, or a failed join when they were never seated.
  /// @param seat The console seat.
  void guest_unseated(controllers::SeatNumber seat) override;
  /// @brief Logs and reports SharingFailed with
  /// Refusal::Kind::unsupported_version.
  /// @param reason The session's reason.
  void became_unshareable(play::NotShareable reason) override;
  /// @brief Runs one pump turn and delivers its steps, on the session's
  /// thread.
  /// @param wait The longest the session intends to wait.
  /// @return How long the session may wait: zero when work is waiting,
  ///     otherwise until the core's next timer, capped at `wait` and, while a
  ///     guest is admitted, at Pacing::admitted_wait.
  auto service(std::chrono::milliseconds wait)
      -> std::chrono::milliseconds override;
  /// @brief Tells the core the stream ended, gives up the console seats,
  /// delivers what is left and reports SharingEnded, unless sharing had
  /// already stopped.
  void session_ended() override;

  /// @brief Runs a pump turn through the gate and delivers its steps.
  void pump();
  /// @brief Translates every output of the core into queued steps, and
  /// updates whether datagrams should wake the pump. Call with the lock held.
  void drain();
  /// @brief Queues InvitationReady.
  /// @param ready The core's output; its text is taken.
  void translate(SharedSession::Core::InvitationReady& ready);
  /// @brief Records the guest's device and queues PlayerIdentified with its
  /// recognition; takes the RememberedDevices lock.
  /// @param bound The core's output.
  void translate(const SharedSession::Core::GuestIdentified& bound);
  /// @brief Queues CodeToReadOut.
  /// @param code The core's output.
  void translate(const SharedSession::Core::CodeReady& code);
  /// @brief Updates the guest's summary and queues PlayerChanged.
  /// @param state The core's output.
  void translate(const SharedSession::Core::GuestState& state);
  /// @brief Queues a Joining step.
  /// @param join The core's output.
  void translate(const SharedSession::Core::JoinRequested& join);
  /// @brief Queues a Removal step.
  /// @param removal The core's output.
  void translate(const SharedSession::Core::RemovalRequested& removal);
  /// @brief Queues an Input step.
  /// @param input The core's output.
  void translate(const SharedSession::Core::ControllerInput& input);
  /// @brief Queues a Keyframe step.
  /// @param requested The core's output.
  void translate(const SharedSession::Core::IdrRequested& requested);
  /// @brief Queues SeatsChanged.
  /// @param changed The core's output.
  void translate(const SharedSession::Core::SeatsChanged& changed);
  /// @brief Queues a Note.
  /// @param diagnostic The core's output; its text is taken.
  void translate(SharedSession::Core::Diagnostic& diagnostic);
  /// @brief Hands an event to the observer, unless sharing stopped; logs
  /// guest progress, and marks a newly verified guest's remembered device as
  /// seen (which blocks while the store saves).
  /// @param event The event.
  void deliver(SharedSession::Event event);
  /// @brief Asks the session for a console seat for the guest, as they asked
  /// to be seated, and reports the reservation; a refused request fails the
  /// join.
  /// @param joining The step.
  void deliver(const Joining& joining);
  /// @brief Gives up the guest's console seat and reports PlayerUnseated if
  /// they had been seated.
  /// @param removal The step.
  void deliver(const Removal& removal);
  /// @brief Sends the guest's state through their console seat once seated.
  /// @param input The step.
  void deliver(const Input& input);
  /// @brief Asks the session for a keyframe.
  /// @param keyframe The step.
  void deliver(const Keyframe& keyframe);
  /// @brief Logs a note no longer than SharedSession::diagnostic_limit.
  /// @param note The step.
  void deliver(const Note& note);
  /// @brief Tells the core the console could not seat the guest in a shared
  /// play seat.
  /// @param grant The Kakusan seat.
  void join_failed(const SeatGrant& grant);
  /// @brief Asks the session to call service() soon.
  void wake() const noexcept;
  /// @brief Writes a log line.
  /// @param severity How serious it is.
  /// @param message The text.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

  /// @brief The hosting that owns this object.
  SharedSession::Hosting& hosting_;
  /// @brief The Kakusan context.
  Surroundings& surroundings_;
  /// @brief The protocol core.
  SharedSession::Core& machine_;
  /// @brief Receives the events.
  SharedSession::Observer& observer_;
  /// @brief Timing and limits.
  SharedSession::Pacing pacing_;
  /// @brief The console's haptic intensity setting that rumble is scaled by;
  /// guarded by the hosting's lock.
  controllers::FeedbackIntensity haptic_intensity_{
      controllers::FeedbackIntensity::strong};
};

}  // namespace usecases
}  // namespace libchiaking::kakusan
