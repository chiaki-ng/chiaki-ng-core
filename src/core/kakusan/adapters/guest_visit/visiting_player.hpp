/// @file
/// @brief Declares VisitingPlayer, the guest's state machine for one visit,
/// from the link coming up to playing and leaving.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/kakusan/adapters/player_channel.hpp"
#include "src/core/kakusan/model/guest_progress.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::kakusan {

/// @brief The guest's side of a visit, as a state machine fed events and
/// answering with effects.
///
/// After the shared steps of PlayerChannel (introduction and verification),
/// the guest side follows the host's lead:
///
/// - AdmissionPending starts admission_timeout_us to wait for the source
///   setup;
/// - the source setup, which must be for the stream protocol the invitation
///   named, opens the forwarded stream; once open, SourceReady is sent with
///   the setup's hash;
/// - SeatGranted moves to Progress::getting_ready and asks for the
///   controller; once ready, Ready is sent;
/// - Active moves to Progress::playing and starts media reception. Input is
///   sent paced and limited to the allowed buttons; rumble from the host is
///   applied, and stopped after rumble_stop_us without a fresh frame.
///
/// Ending the visit stops the stream and any rumble.
///
/// Pure: it does no I/O and reads no clock; time arrives in the events. Owned
/// by GuestVisit::Core and driven through an EffectLoop under the visit's
/// lock; not thread-safe and neither copyable nor movable.
class VisitingPlayer {
 public:
  /// @brief Everything the guest feeds the machine.
  using Event =
      std::variant<IceNominated, DtlsEstablished, TransportFailed,
                   InvitationExpired, ControlReceived, ControlAcked,
                   MalformedData, LocalFailure, LocalCode, PeerCodeEntered,
                   Cancel, ReceiverReady, ReceiverFailed, ControllerReady,
                   FeedbackReceived, ControllerSampled, IdrNeeded>;
  /// @brief One thing the machine asks the guest to do.
  using Effect = VisitingEffect;
  /// @brief The effects of one step, in the order to apply them.
  using Effects = std::vector<Effect>;

  /// @brief Builds a machine at Progress::waiting for an accepted invitation.
  /// @param crypto Crypto for verification; must outlive the machine.
  /// @param random Randomness for verification and pings; must outlive the
  ///     machine.
  /// @param context What the guest shares with the host, from its reply.
  /// @param source_protocol The stream protocol version the invitation named;
  ///     the source setup must match it.
  VisitingPlayer(platform::CryptoProvider& crypto,
                 platform::SecureRandomProvider& random,
                 const PairingContext& context,
                 std::uint8_t source_protocol) noexcept;
  /// @brief Not copyable; it holds secrets of one visit.
  VisitingPlayer(const VisitingPlayer&) = delete;
  /// @brief Not copy-assignable; it holds secrets of one visit.
  VisitingPlayer& operator=(const VisitingPlayer&) = delete;
  /// @brief Not movable; the channel is referred to by address.
  VisitingPlayer(VisitingPlayer&&) = delete;
  /// @brief Not move-assignable; the channel is referred to by address.
  VisitingPlayer& operator=(VisitingPlayer&&) = delete;

  /// @brief Advances the clock to the event's time, handles any timers that
  /// fell due, then handles the event.
  /// @param event The event.
  /// @return The effects to apply.
  auto handle(Event event) -> Effects;
  /// @brief Advances the clock and handles timers that fell due.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return The effects to apply.
  auto on_time(std::uint64_t now_us) -> Effects;
  /// @brief The machine's earliest timer, including the next input frame
  /// while playing.
  /// @return In monotonic microseconds, or nothing when none is set.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief The stage the visit has reached.
  /// @return The stage.
  auto state() const noexcept -> Progress { return channel_.state(); }
  /// @brief The seat the host granted.
  /// @return The binding, once SeatGranted arrived.
  auto binding() const noexcept -> std::optional<net::kakusan::SeatBinding>;

 private:
  /// @brief Moves the clock forward and handles timers.
  /// @param now_us The current monotonic time, in microseconds.
  void advance(std::uint64_t now_us);
  /// @brief Handles every timer that fell due: finishes a leaving visit,
  /// fails on a link, hello, pairing or admission timeout, leaves a silent
  /// link, stops a stale rumble, sends a ping and sends due input.
  void expire();

  /// @brief Starts connecting once a path to the host is found.
  /// @param event The event.
  void on(IceNominated& event);
  /// @brief Starts connecting if not yet, then the introduction over the new
  /// link.
  /// @param event The event.
  void on(DtlsEstablished& event);
  /// @brief Ends the visit, or finishes it when already leaving.
  /// @param event The event.
  void on(TransportFailed& event);
  /// @brief Fails the visit as expired if the host never connected.
  /// @param event The event.
  void on(InvitationExpired& event);
  /// @brief Dispatches a control message to its receive() overload; ignored
  /// before the link is up or once ending.
  /// @param event The event.
  void on(ControlReceived& event);
  /// @brief Finishes a leaving visit once its last message is acknowledged.
  /// @param event The event.
  void on(ControlAcked& event);
  /// @brief Fails the visit once the link is up: a codec error stays one,
  /// anything else is a protocol error.
  /// @param event The event.
  void on(MalformedData& event);
  /// @brief Fails the visit with the given error.
  /// @param event The event.
  void on(LocalFailure& event);
  /// @brief Shows the guest's code, and answers the host's share if the
  /// host's code is in.
  /// @param event The event.
  void on(LocalCode& event);
  /// @brief Keeps the host's code, and answers the host's share if the
  /// guest's code is drawn.
  /// @param event The event.
  void on(PeerCodeEntered& event);
  /// @brief Ends the visit, telling a verified host the guest left.
  /// @param event The event.
  void on(Cancel& event);
  /// @brief Sends SourceReady with the setup's hash once the stream opened.
  /// @param event The event.
  void on(ReceiverReady& event);
  /// @brief Fails the visit with a codec error when the stream could not be
  /// opened.
  /// @param event The event.
  void on(ReceiverFailed& event);
  /// @brief Sends Ready once the controller is ready.
  /// @param event The event.
  void on(ControllerReady& event);
  /// @brief Applies a fresh rumble frame and restarts or clears the rumble
  /// timer; fails on a wrong seat.
  /// @param event The event.
  void on(FeedbackReceived& event);
  /// @brief Keeps the latest valid controller state and sends it if due.
  /// @param event The event.
  void on(ControllerSampled& event);
  /// @brief Asks the host for a keyframe while playing.
  /// @param event The event.
  void on(IdrNeeded& event);

  /// @brief Handles the host's Hello.
  /// @param message The message.
  void receive(const net::kakusan::control::Hello& message);
  /// @brief Handles the host's identity proof.
  /// @param message The message.
  void receive(const net::kakusan::control::Identity& message);
  /// @brief Keeps the host's share and answers it if both codes are in; out
  /// of order it counts as a failed verification.
  /// @param message The message.
  void receive(const net::kakusan::control::PakeA& message);
  /// @brief Checks the host's confirmation and sends ConfirmB; a mismatch is
  /// a failed verification.
  /// @param message The message.
  void receive(const net::kakusan::control::ConfirmA& message);
  /// @brief Sends Authenticated back and is admitted.
  /// @param message The message.
  void receive(const net::kakusan::control::Authenticated& message);
  /// @brief Starts waiting for the source setup.
  /// @param message The message.
  void receive(const net::kakusan::control::AdmissionPending& message);
  /// @brief Opens the forwarded stream from the setup, which is taken; fails
  /// on a different stream protocol or an unusable setup.
  /// @param message The message.
  void receive(net::kakusan::control::SourceSetup& message);
  /// @brief Takes the grant, moves to getting ready and asks for the
  /// controller.
  /// @param message The message.
  void receive(const net::kakusan::control::SeatGranted& message);
  /// @brief Starts playing.
  /// @param message The message.
  void receive(const net::kakusan::control::Active& message);
  /// @brief Ends the visit because the host took the seat back.
  /// @param message The message.
  void receive(const net::kakusan::control::SeatRevoked& message);
  /// @brief Ends the visit because the host ended it.
  /// @param message The message.
  void receive(const net::kakusan::control::End& message);
  /// @brief Answers the host's ping.
  /// @param message The message.
  void receive(const net::kakusan::control::Ping& message);
  /// @brief Takes the host's answer to a ping.
  /// @param message The message.
  void receive(const net::kakusan::control::Pong& message);
  /// @brief Ends the visit because the host reported a failure.
  /// @param message The message.
  void receive(const net::kakusan::control::Failure& message);
  /// @brief Ends the visit: only the guest sends PakeB.
  /// @param message The message.
  void receive(const net::kakusan::control::PakeB& message);
  /// @brief Ends the visit: only the guest sends ConfirmB.
  /// @param message The message.
  void receive(const net::kakusan::control::ConfirmB& message);
  /// @brief Fails the visit with a protocol error: the host sent a message
  /// the guest never expects.
  /// @tparam Message Any other control message type.
  /// @param message The message.
  template <class Message>
  void receive(const Message& message);

  /// @brief The latest controller state limited to the granted buttons. A
  /// grant must be held.
  /// @return The state to send.
  auto allowed_input() const noexcept -> net::kakusan::InputState;
  /// @brief Moves to connecting and starts waiting for the encrypted link.
  void connect();
  /// @brief Once both codes and the host's share are in, completes the code
  /// exchange and sends PakeB.
  void answer_pake();
  /// @brief Moves to playing, starts media reception and input pacing.
  void activate();
  /// @brief Sends an input frame with the allowed state when the pacer says
  /// one is due.
  void pump_input();

  /// @brief Ends the visit with a failure, told to the host when the link is
  /// up.
  /// @param error Why.
  void fail(net::kakusan::Error error);
  /// @brief Ends the visit normally, sending the host Leave.
  /// @param reason Why.
  void leave(net::kakusan::control::Reason reason);
  /// @brief Ends the visit when an ending is given.
  /// @param ending The ending, or nothing to carry on.
  void conclude(std::optional<Ending> ending);
  /// @brief Ends the visit: stops the stream and rumble, stops the guest's
  /// timers and hands over to PlayerChannel::end. Does nothing once ending.
  /// @param ending How it ends.
  void terminate(Ending ending);

  /// @brief Introduction, verification, liveness and ending.
  PlayerChannel<Effect> channel_;
  /// @brief The stream protocol version the invitation named.
  std::uint8_t source_protocol_{};

  /// @brief How far the guest has got beyond the channel, over the channel's
  /// outbox.
  player_lifecycle::GuestProgress progress_;
  /// @brief The host's PakeA share, until answered.
  std::optional<net::kakusan::control::Share> host_share_;
  /// @brief The hash of the source setup, sent in SourceReady.
  net::kakusan::Digest256 setup_hash_{};
  /// @brief The seat the host granted, with its counts and allowed buttons.
  std::optional<net::kakusan::control::SeatGranted> grant_;

  /// @brief When the wait for the source setup times out, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> admission_deadline_;
  /// @brief When a nonzero rumble is stopped for want of a fresh frame, in
  /// monotonic microseconds.
  std::optional<std::uint64_t> rumble_deadline_;

  /// @brief When playing began, in monotonic microseconds.
  std::uint64_t active_at_{};
  /// @brief The latest controller state from the frontend.
  net::kakusan::InputState current_input_{};
  /// @brief The state last sent.
  net::kakusan::InputState sent_input_{};
  /// @brief Paces input frames: soon after a change, periodically otherwise.
  net::kakusan::StatePacer input_pacer_;
  /// @brief The sequence number of the next input frame.
  std::uint32_t input_sequence_{};
  /// @brief Drops feedback frames older than the newest one seen.
  net::kakusan::Freshness feedback_freshness_;
};

}  // namespace libchiaking::kakusan
