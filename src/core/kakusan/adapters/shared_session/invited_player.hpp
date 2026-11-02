/// @file
/// @brief Declares InvitedPlayer, the host's state machine for one invited
/// guest, from the reply being imported to the guest playing and leaving.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/kakusan/adapters/player_channel.hpp"
#include "src/core/kakusan/model/host_progress.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::kakusan {

/// @brief The host's view of one guest's visit, as a state machine fed events
/// and answering with effects.
///
/// After the shared steps of PlayerChannel (introduction and verification),
/// the host side goes on to seat the guest:
///
/// - once verified and approved by the host, a seat is reserved and the
///   guest told admission is pending (admission_timeout_us);
/// - once the console seats the guest, the source setup is sent; after it is
///   acknowledged the guest has source_ready_timeout_us to answer SourceReady
///   with the setup's hash;
/// - the seat is then granted and SeatGranted sent; after it is acknowledged
///   the guest has ready_timeout_us to answer Ready;
/// - Active is sent and the guest is playing; once Active is acknowledged,
///   media starts. Input is passed on limited to the allowed buttons; after
///   input_neutral_us without input a neutral state is sent, after
///   input_revoke_us the seat is taken back. Rumble is sent back paced.
///
/// Ending a visit undoes in reverse whatever was set up: media, the console
/// seat and the Kakusan seat.
///
/// Pure: it does no I/O and reads no clock; time arrives in the events. One
/// per guest, owned by SharedSession::Core and driven through an EffectLoop
/// under the session's lock; not thread-safe and neither copyable nor
/// movable.
class InvitedPlayer {
 public:
  /// @brief Everything the host feeds the machine.
  using Event =
      std::variant<ReplyImported, IceNominated, DtlsEstablished,
                   TransportFailed, InvitationExpired, ControlReceived,
                   ControlAcked, MalformedData, LocalFailure, LocalCode,
                   PeerCodeEntered, Cancel, AdmissionApproved, SeatReserved,
                   NoCapacity, SourceJoined, SourceJoinFailed, CapacityRevoked,
                   SourceEnded, InputReceived, RumbleSampled>;
  /// @brief One thing the machine asks the host to do.
  using Effect = InvitedEffect;
  /// @brief The effects of one step, in the order to apply them.
  using Effects = std::vector<Effect>;

  /// @brief Builds a machine at Progress::waiting, before any reply.
  /// @param crypto Crypto for verification; must outlive the machine.
  /// @param random Randomness for verification and pings; must outlive the
  ///     machine.
  InvitedPlayer(platform::CryptoProvider& crypto,
                platform::SecureRandomProvider& random) noexcept;
  /// @brief Not copyable; it holds secrets of one visit.
  InvitedPlayer(const InvitedPlayer&) = delete;
  /// @brief Not copy-assignable; it holds secrets of one visit.
  InvitedPlayer& operator=(const InvitedPlayer&) = delete;
  /// @brief Not movable; the channel is referred to by address.
  InvitedPlayer(InvitedPlayer&&) = delete;
  /// @brief Not move-assignable; the channel is referred to by address.
  InvitedPlayer& operator=(InvitedPlayer&&) = delete;

  /// @brief Advances the clock to the event's time, handles any timers that
  /// fell due, then handles the event.
  /// @param event The event.
  /// @return The effects to apply.
  auto handle(Event event) -> Effects;
  /// @brief Advances the clock and handles timers that fell due.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return The effects to apply.
  auto on_time(std::uint64_t now_us) -> Effects;
  /// @brief The machine's earliest timer.
  /// @return In monotonic microseconds, or nothing when none is set.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief The stage the guest has reached.
  /// @return The stage.
  auto state() const noexcept -> Progress { return channel_.state(); }
  /// @brief The guest's Kakusan seat.
  /// @return The binding, once a seat has been reserved; kept after ending.
  auto binding() const noexcept -> std::optional<net::kakusan::SeatBinding>;

 private:
  /// @brief Moves the clock forward and handles timers.
  /// @param now_us The current monotonic time, in microseconds.
  void advance(std::uint64_t now_us);
  /// @brief Handles every timer that fell due: finishes a leaving guest,
  /// fails on a connectivity, link, hello, pairing, admission or source-ready
  /// timeout, takes the seat back on a ready or input timeout, ends a silent
  /// link, sends a neutral state, sends a ping, and sends due feedback.
  void expire();

  /// @brief Takes the shared context and starts connecting, with the ICE
  /// connectivity timeout; only at Progress::waiting.
  /// @param event The event.
  void on(ReplyImported& event);
  /// @brief Stops the connectivity timeout and starts waiting for the
  /// encrypted link.
  /// @param event The event.
  void on(IceNominated& event);
  /// @brief Starts the introduction over the new link; ignored unless
  /// connecting with the context known.
  /// @param event The event.
  void on(DtlsEstablished& event);
  /// @brief Ends the visit, or finishes it when already leaving.
  /// @param event The event.
  void on(TransportFailed& event);
  /// @brief Fails the visit as expired if no reply came.
  /// @param event The event.
  void on(InvitationExpired& event);
  /// @brief Dispatches a control message to its receive() overload; ignored
  /// before the link is up or once ending.
  /// @param event The event.
  void on(ControlReceived& event);
  /// @brief Starts the source-ready or ready wait once the setup or grant is
  /// acknowledged, and starts media once Active is.
  /// @param event The event.
  void on(ControlAcked& event);
  /// @brief Fails the visit with a protocol error once the link is up.
  /// @param event The event.
  void on(MalformedData& event);
  /// @brief Fails the visit with the given error.
  /// @param event The event.
  void on(LocalFailure& event);
  /// @brief Shows the host's code, and begins the code exchange if the
  /// guest's code is in.
  /// @param event The event.
  void on(LocalCode& event);
  /// @brief Keeps the guest's code, and begins the code exchange if the
  /// host's code is drawn.
  /// @param event The event.
  void on(PeerCodeEntered& event);
  /// @brief Ends the visit, telling a verified guest; the host removed them.
  /// @param event The event.
  void on(Cancel& event);
  /// @brief Records the host's approval and reserves a seat once verified.
  /// @param event The event.
  void on(AdmissionApproved& event);
  /// @brief Takes the reserved seat, tells the guest admission is pending and
  /// asks the console to seat them; a reservation that is not wanted is
  /// released.
  /// @param event The event.
  void on(SeatReserved& event);
  /// @brief Fails the visit when the wanted reservation could not be made.
  /// @param event The event.
  void on(NoCapacity& event);
  /// @brief Sends the source setup once the console seated the guest; fails
  /// if the setup or the offer is invalid.
  /// @param event The event.
  void on(SourceJoined& event);
  /// @brief Fails the visit when the console would not seat the guest.
  /// @param event The event.
  void on(SourceJoinFailed& event);
  /// @brief Takes the guest's seat back after the seat counts shrank.
  /// @param event The event.
  void on(CapacityRevoked& event);
  /// @brief Ends the visit because the stream or the console seat is gone.
  /// @param event The event.
  void on(SourceEnded& event);
  /// @brief Passes a fresh input frame on to the console, limited to the
  /// allowed buttons; fails on a wrong seat or invalid input.
  /// @param event The event.
  void on(InputReceived& event);
  /// @brief Keeps the latest rumble and sends it if due.
  /// @param event The event.
  void on(RumbleSampled& event);

  /// @brief Handles the guest's Hello.
  /// @param message The message.
  void receive(const net::kakusan::control::Hello& message);
  /// @brief Handles the guest's identity proof.
  /// @param message The message.
  void receive(const net::kakusan::control::Identity& message);
  /// @brief Completes the code exchange with the guest's share and sends
  /// ConfirmA; out of order it counts as a failed verification.
  /// @param message The message.
  void receive(const net::kakusan::control::PakeB& message);
  /// @brief Checks the guest's confirmation and sends Authenticated; a
  /// mismatch is a failed verification.
  /// @param message The message.
  void receive(const net::kakusan::control::ConfirmB& message);
  /// @brief Admits the guest after their Authenticated, and reserves a seat
  /// if approved.
  /// @param message The message.
  void receive(const net::kakusan::control::Authenticated& message);
  /// @brief Grants the seat and sends SeatGranted once the guest confirms the
  /// setup by its hash.
  /// @param message The message.
  void receive(const net::kakusan::control::SourceReady& message);
  /// @brief Sends Active and moves to playing once the guest's controller is
  /// ready.
  /// @param message The message.
  void receive(const net::kakusan::control::Ready& message);
  /// @brief Asks the console for a keyframe on the guest's behalf.
  /// @param message The message.
  void receive(const net::kakusan::control::SourceIdr& message);
  /// @brief Answers the guest's ping.
  /// @param message The message.
  void receive(const net::kakusan::control::Ping& message);
  /// @brief Takes the guest's answer to a ping.
  /// @param message The message.
  void receive(const net::kakusan::control::Pong& message);
  /// @brief Ends the visit because the guest left.
  /// @param message The message.
  void receive(const net::kakusan::control::Leave& message);
  /// @brief Ends the visit because the guest reported a failure.
  /// @param message The message.
  void receive(const net::kakusan::control::Failure& message);
  /// @brief Ends the visit: only the host sends PakeA.
  /// @param message The message.
  void receive(const net::kakusan::control::PakeA& message);
  /// @brief Ends the visit: only the host sends ConfirmA.
  /// @param message The message.
  void receive(const net::kakusan::control::ConfirmA& message);
  /// @brief Fails the visit with a protocol error: the guest sent a message
  /// the host never expects.
  /// @tparam Message Any other control message type.
  /// @param message The message.
  template <class Message>
  void receive(const Message& message);

  /// @brief Checks a seat grant before it is offered.
  /// @param offer The grant.
  /// @return True when the seat is within the capacity, the generation and
  ///     local seat count are sensible, and the buttons are known and include
  ///     the required ones.
  static auto valid_offer(
      const net::kakusan::control::SeatGranted& offer) noexcept -> bool;
  /// @brief Tells whether the rumble differs from the last one sent.
  /// @return True when it does.
  auto rumble_changed() const noexcept -> bool;
  /// @brief Tells whether either motor is on.
  /// @return True when one is.
  auto rumbling() const noexcept -> bool;
  /// @brief Starts the code exchange and sends PakeA once both codes are in.
  void begin_pairing();
  /// @brief Asks for a seat once the guest is verified and approved, once.
  void maybe_reserve();
  /// @brief Sends Active, starts the input timers and moves to playing.
  void activate();
  /// @brief Starts forwarding the stream and pacing feedback, once.
  void start_media();
  /// @brief Sends a feedback frame with the current rumble when the pacer
  /// says one is due.
  void pump_feedback();

  /// @brief Ends the visit with a failure, told to the guest when the link is
  /// up.
  /// @param error Why.
  void fail(net::kakusan::Error error);
  /// @brief Ends the visit normally, sending the guest End.
  /// @param reason Why.
  void end(net::kakusan::control::Reason reason);
  /// @brief Takes the seat back: sends SeatRevoked once granted, otherwise
  /// fails the visit (no capacity or timeout).
  /// @param reason Why.
  void revoke(net::kakusan::control::Reason reason);
  /// @brief Ends the visit when an ending is given.
  /// @param ending The ending, or nothing to carry on.
  void conclude(std::optional<Ending> ending);
  /// @brief Ends the visit: stops media, sends a neutral state, removes the
  /// console seat, releases the Kakusan seat, stops the host's timers and
  /// hands over to PlayerChannel::end. Ending part way through verification
  /// counts as a failed verification. Does nothing once ending.
  /// @param ending How it ends.
  void terminate(Ending ending);

  /// @brief Introduction, verification, liveness and ending.
  PlayerChannel<Effect> channel_;

  /// @brief How far the guest has got beyond the channel, over the
  /// channel's outbox.
  player_lifecycle::HostProgress progress_;
  /// @brief The code exchange in progress, from PakeA until PakeB.
  std::optional<net::kakusan::Exchange> exchange_;
  /// @brief The grant sent to the guest, including the allowed buttons.
  std::optional<net::kakusan::control::SeatGranted> offer_;
  /// @brief The hash of the source setup sent, which SourceReady must echo.
  net::kakusan::Digest256 setup_hash_{};
  /// @brief The token of the source setup until acknowledged; zero otherwise.
  net::kakusan::AckToken setup_token_{};
  /// @brief The token of SeatGranted until acknowledged; zero otherwise.
  net::kakusan::AckToken grant_token_{};
  /// @brief The token of Active until acknowledged; zero otherwise.
  net::kakusan::AckToken active_token_{};

  /// @brief When finding a network path times out, in monotonic microseconds.
  std::optional<std::uint64_t> connectivity_deadline_;
  /// @brief When seating the guest times out, in monotonic microseconds.
  std::optional<std::uint64_t> admission_deadline_;
  /// @brief When the wait for SourceReady times out, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> source_ready_deadline_;
  /// @brief When the wait for Ready times out, in monotonic microseconds.
  std::optional<std::uint64_t> ready_deadline_;

  /// @brief When the guest started playing, in monotonic microseconds.
  std::uint64_t active_at_{};
  /// @brief When the guest's last input arrived, in monotonic microseconds.
  std::uint64_t last_input_{};
  /// @brief Drops input frames older than the newest one seen.
  net::kakusan::Freshness input_freshness_;

  /// @brief Paces feedback frames: soon after a change, periodically while
  /// rumbling.
  net::kakusan::StatePacer feedback_pacer_;
  /// @brief The sequence number of the next feedback frame.
  std::uint32_t feedback_sequence_{};
  /// @brief The console's current low-frequency rumble, 0 to 65535.
  std::uint16_t rumble_low_{};
  /// @brief The console's current high-frequency rumble, 0 to 65535.
  std::uint16_t rumble_high_{};
  /// @brief The low-frequency rumble last sent.
  std::uint16_t sent_low_{};
  /// @brief The high-frequency rumble last sent.
  std::uint16_t sent_high_{};
};

}  // namespace libchiaking::kakusan
