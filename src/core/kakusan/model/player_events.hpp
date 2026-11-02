/// @file
/// @brief Declares the vocabulary of the per-player state machines: their
/// timeouts, the events fed to them and the effects they answer with.
///
/// The host keeps one InvitedPlayer per guest and the guest one VisitingPlayer
/// for its visit. Both are pure state machines: their owners
/// (SharedSession::Core and GuestVisit::Core) turn network, console and user
/// happenings into the events below, and carry out the effects the machines
/// return. Every event carries the monotonic time, in microseconds, at which it
/// happened.
#pragma once

#include "libchiaking/detail/kakusan/values.hpp"
#include "src/core/net/kakusan/ack_token.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/identifiers.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/timing.hpp"
#include "src/core/net/kakusan/verification_code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace libchiaking::kakusan {

/// @brief How long after the ICE path is nominated the encrypted link may take
/// to come up, in microseconds: 15 s.
inline constexpr std::uint64_t dtls_timeout_us = 15'000'000;
/// @brief How long after the link comes up the peer's Hello and Identity may
/// take to arrive, in microseconds: 10 s.
inline constexpr std::uint64_t hello_timeout_us = 10'000'000;
/// @brief How long the two people have, once the peer is identified, to read
/// out and enter each other's codes and finish verifying, in microseconds:
/// five minutes.
inline constexpr std::uint64_t pairing_timeout_us = 300'000'000;
/// @brief How long a verified guest may wait, once a seat is reserved for
/// them, for the console to seat them and the source setup to be sent, in
/// microseconds: 60 s.
inline constexpr std::uint64_t admission_timeout_us = 60'000'000;
/// @brief How long the guest may take, after acknowledging the source setup,
/// to report that it can decode the stream, in microseconds: 15 s.
inline constexpr std::uint64_t source_ready_timeout_us = 15'000'000;
/// @brief How long the guest may take, after acknowledging the seat grant, to
/// report its controller ready, in microseconds: 15 s.
inline constexpr std::uint64_t ready_timeout_us = 15'000'000;
/// @brief How long a playing guest's input may go silent before the host sends
/// the console a neutral controller state for their seat, in microseconds:
/// 250 ms.
inline constexpr std::uint64_t input_neutral_us = 250'000;
/// @brief How long a playing guest's input may go silent before the host takes
/// their seat back, in microseconds: 6 s.
inline constexpr std::uint64_t input_revoke_us = 6'000'000;
/// @brief How long the guest keeps a nonzero rumble going without a fresh
/// feedback frame before stopping it, in microseconds: 250 ms.
inline constexpr std::uint64_t rumble_stop_us = 250'000;
/// @brief How long a player that is leaving waits for its final message to be
/// acknowledged before closing the link anyway, in microseconds: 1 s.
inline constexpr std::uint64_t terminal_grace_us = 1'000'000;

/// @brief The id of an invitation, which is also the id the host knows the
/// guest by (GuestId).
using InvitationId = net::kakusan::InvitationId;

/// @brief What both sides must agree on before they verify each other: the
/// hash of the exchanged invitation and reply, both identities and the
/// invitation id.
///
/// Plain data. The host builds it when a reply is imported, the guest when it
/// accepts an invitation; the peer's identity in `parties` is filled in once
/// the peer has proved it.
struct PairingContext {
  /// @brief The hash of the invitation and reply, binding every later step
  /// to this exchange.
  net::kakusan::Transcript transcript{};
  /// @brief The host's and guest's identity keys; the peer's starts empty.
  net::kakusan::Parties parties{};
  /// @brief The invitation being answered.
  InvitationId invitation_id{};
  /// @brief The short hint of the peer's identity key carried in the
  /// exchanged text, which the proved key must match.
  net::kakusan::IdentityHint peer_hint{};
};

/// @brief How far the two sides have got in introducing themselves over a
/// freshly established link.
enum class Introduction : std::uint8_t {
  /// @brief Waiting for the peer's Hello, which echoes the transcript.
  awaiting_hello,
  /// @brief Hello received; waiting for the peer's signed Identity.
  awaiting_identity,
  /// @brief The peer's identity is proved.
  complete,
};

/// @brief The link found a working network path to the peer.
struct IceNominated {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief The encrypted link to the peer is up.
struct DtlsEstablished {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief Keying material exported from the link, bound into the
  /// identity proofs and the code exchange.
  net::kakusan::Exporter exporter;
  /// @brief This side's signed identity proof, sent after the peer's Hello.
  net::kakusan::control::Identity proof;
};
/// @brief The link to the peer failed or closed.
struct TransportFailed {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief The invitation's lifetime ran out before a reply was used.
struct InvitationExpired {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief A control message arrived from the peer.
struct ControlReceived {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The message.
  net::kakusan::control::Message message;
};
/// @brief The peer acknowledged a control message this side sent.
struct ControlAcked {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The token the message was sent with; zero for none.
  net::kakusan::AckToken token{};
};
/// @brief The peer sent something that could not be read.
struct MalformedData {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief What was wrong with it.
  net::kakusan::Error error{net::kakusan::Error::protocol};
};
/// @brief Something on this side failed on the player's behalf.
struct LocalFailure {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief What failed.
  net::kakusan::Error error{net::kakusan::Error::internal};
};
/// @brief This side's verification code was drawn, for the user to read out.
struct LocalCode {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The code.
  net::kakusan::Code code;
};
/// @brief The user entered the code the other person read out.
struct PeerCodeEntered {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The code.
  net::kakusan::Code code;
};
/// @brief The user, or the owner on their behalf, ended the visit.
struct Cancel {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief Why, as told to the peer.
  net::kakusan::control::Reason reason{net::kakusan::control::Reason::user};
};

/// @brief Host only: the guest's reply was accepted for this invitation.
struct ReplyImported {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief What the two sides now share.
  PairingContext context;
};
/// @brief Host only: the host approved the guest for a seat.
struct AdmissionApproved {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Host only: a Kakusan seat was reserved for the guest.
struct SeatReserved {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: no seat could be reserved for the guest.
struct NoCapacity {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Host only: the console seated the guest, and the source setup to
/// send them is ready.
struct SourceJoined {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The setup to send.
  net::kakusan::control::SourceSetup setup;
  /// @brief The total number of seats, offered in the grant.
  std::uint8_t capacity{};
  /// @brief How many of them the host keeps, offered in the grant.
  std::uint8_t local_seats{};
  /// @brief The wire buttons the guest may press.
  std::uint32_t allowed_buttons{net::kakusan::button_mask};
};
/// @brief Host only: the console would not seat the guest.
struct SourceJoinFailed {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Host only: the guest's seat no longer exists after the seat counts
/// changed.
struct CapacityRevoked {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Host only: the stream being shared ended, or the guest's console seat
/// was lost.
struct SourceEnded {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Host only: an input frame arrived from the guest.
struct InputReceived {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The frame.
  net::kakusan::InputFrame frame;
};
/// @brief Host only: the console set the rumble for the guest's seat.
struct RumbleSampled {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief Low-frequency motor strength, 0 to 65535.
  std::uint16_t low_frequency{};
  /// @brief High-frequency motor strength, 0 to 65535.
  std::uint16_t high_frequency{};
};

/// @brief Guest only: the forwarded stream was opened from the source setup.
struct ReceiverReady {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Guest only: the forwarded stream could not be opened.
struct ReceiverFailed {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Guest only: the guest's controller is ready for the granted seat.
struct ControllerReady {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};
/// @brief Guest only: a feedback (rumble) frame arrived from the host.
struct FeedbackReceived {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The frame.
  net::kakusan::FeedbackFrame frame;
};
/// @brief Guest only: the guest's frontend sent a new controller state.
struct ControllerSampled {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
  /// @brief The state, in wire form.
  net::kakusan::InputState state;
};
/// @brief Guest only: the guest's decoder needs a keyframe.
struct IdrNeeded {
  /// @brief When it happened, in monotonic microseconds.
  std::uint64_t now_us{};
};

/// @brief Send a control message to the peer.
struct SendControl {
  /// @brief The message.
  net::kakusan::control::Message message;
  /// @brief The token its acknowledgement comes back with.
  net::kakusan::AckToken token{};
  /// @brief True for the last message before leaving; a failure to send it
  /// is not reported back.
  bool terminal{};
};
/// @brief Report the player's progress to the user.
struct ShowState {
  /// @brief The stage reached.
  Progress state{Progress::waiting};
  /// @brief Why the visit failed, once it has.
  std::optional<net::kakusan::Error> error;
  /// @brief Why the visit ended, once an ending was announced.
  std::optional<net::kakusan::control::Reason> reason;
  /// @brief True when the other side ended or failed it.
  bool by_peer{};
};
/// @brief Report the peer's proved identity.
struct PeerIdentified {
  /// @brief The peer's identity key.
  net::kakusan::IdentityKey key{};
  /// @brief The host's session details from its proof; only when the
  /// peer is the host.
  std::optional<net::kakusan::control::Identity::Hosting> hosting;
};
/// @brief Show this side's verification code to the user.
struct DisplayCode {
  /// @brief The code.
  net::kakusan::Code code;
};
/// @brief Wipe the player's secrets kept outside the machine, such as its media
/// sender; the visit is ending.
struct EraseSecrets {};
/// @brief Close the link to the peer; the player is gone.
struct CloseTransport {};
/// @brief Count a failed verification towards the host's cooldown.
struct RecordAuthenticationFailure {};
/// @brief Use up the invitation: the guest was admitted or the attempt is over.
struct ConsumeInvitation {};

/// @brief Host only: reserve a Kakusan seat for the guest.
struct ReserveSeat {};
/// @brief Host only: mark the reserved seat as granted to the guest.
struct GrantSeat {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: give the guest's Kakusan seat back.
struct ReleaseSeat {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: ask the console to seat the guest.
struct RequestSourceJoin {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: ask the console to drop the guest's seat.
struct RequestSourceRemoval {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: start forwarding the stream to the guest.
struct StartMedia {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: stop forwarding the stream to the guest.
struct StopMedia {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: send the guest's input to the console.
struct ApplyInput {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
  /// @brief The input, already limited to the allowed buttons.
  net::kakusan::InputState state;
};
/// @brief Host only: send the console a neutral controller state for the guest.
struct InjectNeutral {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Host only: send a feedback (rumble) frame to the guest.
struct SendFeedback {
  /// @brief The frame.
  net::kakusan::FeedbackFrame frame;
};
/// @brief Host only: ask the console for a keyframe.
struct RequestIdr {};

/// @brief Guest only: open the forwarded stream from the source setup.
struct ConfigureReceiver {
  /// @brief The setup the host sent.
  net::kakusan::control::SourceSetup setup;
};
/// @brief Guest only: start taking the forwarded stream.
struct StartMediaReception {
  /// @brief The guest's seat and its generation.
  net::kakusan::SeatBinding binding;
};
/// @brief Guest only: close the forwarded stream.
struct StopMediaReception {};
/// @brief Guest only: get the guest's controller ready for the granted seat.
struct PrepareController {
  /// @brief The grant: seat, counts and allowed buttons.
  net::kakusan::control::SeatGranted grant;
};
/// @brief Guest only: send an input frame to the host.
struct SendInput {
  /// @brief The frame.
  net::kakusan::InputFrame frame;
};
/// @brief Guest only: set the guest's rumble.
struct ApplyRumble {
  /// @brief Low-frequency motor strength, 0 to 65535.
  std::uint16_t low_frequency{};
  /// @brief High-frequency motor strength, 0 to 65535.
  std::uint16_t high_frequency{};
};
/// @brief Guest only: stop the guest's rumble.
struct StopRumble {};

/// @brief Every effect a VisitingPlayer, the guest's machine, can ask for.
using VisitingEffect =
    std::variant<SendControl, ShowState, PeerIdentified, DisplayCode,
                 EraseSecrets, CloseTransport, RecordAuthenticationFailure,
                 ConsumeInvitation, ConfigureReceiver, StartMediaReception,
                 StopMediaReception, PrepareController, SendInput, ApplyRumble,
                 StopRumble>;
/// @brief Every effect an InvitedPlayer, the host's machine for one guest, can
/// ask for.
using InvitedEffect =
    std::variant<SendControl, ShowState, PeerIdentified, DisplayCode,
                 EraseSecrets, CloseTransport, RecordAuthenticationFailure,
                 ConsumeInvitation, ReserveSeat, GrantSeat, ReleaseSeat,
                 RequestSourceJoin, RequestSourceRemoval, StartMedia, StopMedia,
                 ApplyInput, InjectNeutral, SendFeedback, RequestIdr>;

}  // namespace libchiaking::kakusan
