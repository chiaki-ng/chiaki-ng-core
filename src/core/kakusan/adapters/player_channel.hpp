/// @file
/// @brief Declares PlayerChannel, the part of a player's state machine that
/// host and guest share: introducing themselves over the link, verifying each
/// other with the read-out codes, keeping the link alive and ending the visit.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/kakusan/model/player_lifecycle.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/timing.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::kakusan {

/// @brief How a visit ends: what to tell the peer, and what to show the user.
///
/// Plain data, built through the named constructors below and handed to
/// PlayerChannel::end.
struct Ending {
  /// @brief The last message to send the peer (End, Leave, SeatRevoked or
  /// Failure), or nothing to close without one.
  std::optional<net::kakusan::control::Message> notice;
  /// @brief Why the visit failed; nothing when it ended normally.
  std::optional<net::kakusan::Error> error;
  /// @brief Why the visit ended normally; nothing when it failed.
  std::optional<net::kakusan::control::Reason> reason;
  /// @brief True when the other side ended or failed it.
  bool by_peer{};
  /// @brief True when the ending counts as a failed verification towards
  /// the host's cooldown.
  bool authentication_failure{};

  /// @brief An ending without telling the peer.
  /// @param reason Why.
  /// @return The ending.
  static auto quietly(net::kakusan::control::Reason reason) -> Ending;
  /// @brief A failure without telling the peer, such as an expired
  /// invitation or no network path.
  /// @param error Why.
  /// @return The ending.
  static auto refused(net::kakusan::Error error) -> Ending;
  /// @brief A normal ending told to the peer.
  /// @param notice The message to send.
  /// @param reason Why.
  /// @return The ending.
  static auto announced(net::kakusan::control::Message notice,
                        net::kakusan::control::Reason reason) -> Ending;
  /// @brief A failure, told to the peer as a Failure message when
  /// `announce` is true.
  /// @param error Why.
  /// @param announce Whether the link is up to tell the peer.
  /// @return The ending.
  static auto failed(net::kakusan::Error error, bool announce) -> Ending;
  /// @brief A failed verification: the codes did not match, or the code
  /// exchange was out of order. Told to the peer and counted towards the
  /// cooldown.
  /// @return The ending.
  static auto pairing_failed() -> Ending;
  /// @brief The peer ended the visit normally.
  /// @param reason The reason the peer gave.
  /// @return The ending.
  static auto ended_by_peer(net::kakusan::control::Reason reason) -> Ending;
  /// @brief The peer reported a failure.
  /// @param error The failure the peer reported.
  /// @param guessed True when it is a failed verification reported while
  ///     verifying, which counts towards the cooldown.
  /// @return The ending.
  static auto failed_by_peer(net::kakusan::Error error, bool guessed) -> Ending;
};

/// @brief The shared core of InvitedPlayer and VisitingPlayer.
///
/// Takes a player from a freshly established link to verified, then keeps it
/// alive and ends it:
///
/// 1. Each side sends Hello echoing the transcript, then its signed Identity;
///    the peer's proof is checked against the transcript, the link's exported
///    keying material and the identity hint from the exchanged text.
/// 2. Verifying: each side shows its own code and takes the peer's, then the
///    two run a password-authenticated key exchange keyed by both codes and
///    the invitation id, and confirm it with MACs; a wrong code shows up as a
///    confirmation that does not match.
/// 3. Admitted: both sides exchange Authenticated, the invitation is consumed
///    and pings start.
/// 4. Ending: secrets are wiped, an optional last message is sent, and the
///    player waits up to terminal_grace_us for it to be acknowledged before
///    reaching Progress::gone and closing the link.
///
/// Codes, the code exchange and confirmations are held only as long as needed
/// and wiped on ending. Time only moves forward: advance() ignores earlier
/// readings. Every effect is queued in the outbox of the channel's machine,
/// which the player drains after each step. Not thread-safe; owned by its
/// player and used under its owner's lock.
///
/// @tparam Effect The player's effect variant, VisitingEffect or InvitedEffect;
///     it must hold SendControl, ShowState, PeerIdentified, DisplayCode,
///     EraseSecrets, CloseTransport, RecordAuthenticationFailure and
///     ConsumeInvitation.
template <class Effect>
class PlayerChannel {
 public:
  /// @brief Builds a channel at Progress::waiting.
  ///
  /// @param crypto Crypto for proofs, hashes and the code exchange; must
  ///     outlive the channel.
  /// @param random Randomness for the exchange and ping nonces; must
  ///     outlive the channel.
  /// @param role Which side this is.
  /// @param context What both sides share; the guest knows it from the
  ///     start, the host adopts it when a reply is imported.
  PlayerChannel(platform::CryptoProvider& crypto,
                platform::SecureRandomProvider& random, net::kakusan::Role role,
                std::optional<PairingContext> context) noexcept;

  /// @brief Moves the channel's clock forward.
  /// @param now_us The current monotonic time, in microseconds; an earlier
  ///     reading than the last is ignored.
  void advance(std::uint64_t now_us) noexcept;
  /// @brief The channel's clock.
  /// @return The latest monotonic time seen, in microseconds.
  auto now() const noexcept -> std::uint64_t { return now_; }
  /// @brief The stage the player has reached.
  /// @return The stage.
  auto state() const noexcept -> Progress { return state_; }
  /// @brief Tells whether verification succeeded and the visit is under way.
  /// @return True at Progress::waiting_for_seat, getting_ready and playing.
  auto authenticated() const noexcept -> bool;
  /// @brief Tells whether the visit is ending or over.
  /// @return True at Progress::leaving and Progress::gone.
  auto finished() const noexcept -> bool;
  /// @brief Tells whether the encrypted link has come up.
  /// @return True from establish() on.
  auto open() const noexcept -> bool { return lifecycle_.is_open(); }
  /// @brief The outbox every effect of the player is queued in.
  /// @return The outbox, owned by the channel's machine.
  auto outbox() noexcept -> player_lifecycle::Outbox<Effect>& {
    return lifecycle_.outbox();
  }
  /// @brief Hands over the effects queued since the last drain.
  /// @return The effects, in the order to apply them.
  auto drain() -> std::vector<Effect> { return lifecycle_.outbox().drain(); }
  /// @brief Tells whether the shared context is known yet.
  /// @return False on the host until a reply is imported.
  auto has_context() const noexcept -> bool { return context_.has_value(); }
  /// @brief The channel's earliest timer.
  /// @return The earliest of the link, hello and pairing timeouts and the
  ///     next ping or silence check; the closing deadline while leaving;
  ///     nothing once gone or when no timer is set. In monotonic
  ///     microseconds.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Time elapsed since an earlier instant, for the elapsed field of
  /// input and feedback frames.
  /// @param since_us The earlier instant, in monotonic microseconds.
  /// @return The elapsed time in whole milliseconds, truncated to 32 bits.
  auto milliseconds_since(std::uint64_t since_us) const noexcept
      -> std::uint32_t;
  /// @brief Hashes a source setup, so the guest's SourceReady can name the
  /// setup it accepted.
  /// @param setup The setup.
  /// @return Its hash.
  /// @retval net::kakusan::Error The setup could not be encoded or hashed.
  auto setup_hash(const net::kakusan::control::SourceSetup& setup) const
      -> net::kakusan::Result<net::kakusan::Digest256>;

  /// @brief Takes the shared context, on the host once a reply is imported.
  /// @param context The context.
  void adopt(const PairingContext& context);
  /// @brief Moves to a stage and reports it with ShowState.
  /// @param state The new stage.
  void enter(Progress state);
  /// @brief Queues a control message to the peer with a fresh token.
  /// @param message The message.
  /// @return The token its acknowledgement will carry.
  auto send(net::kakusan::control::Message message) -> net::kakusan::AckToken;
  /// @brief Starts the dtls_timeout_us wait for the encrypted link.
  void await_dtls() noexcept;
  /// @brief Records that the encrypted link is up, keeps this side's proof
  /// for later and sends Hello.
  ///
  /// Stops the link timeout and starts hello_timeout_us. The context must be
  /// known.
  ///
  /// @param event The link's exported keying material and this side's proof;
  ///     the proof is taken.
  void establish(DtlsEstablished& event);

  /// @brief Handles the peer's Hello: checks its transcript and sends this
  /// side's Identity proof.
  /// @param message The Hello.
  /// @return Nothing on success, or the ending: a protocol failure when not
  ///     connecting, not waiting for Hello or the link is not up; a
  ///     transcript failure when the transcripts differ.
  auto greet(const net::kakusan::control::Hello& message)
      -> std::optional<Ending>;
  /// @brief Handles the peer's Identity: verifies the proof, records the
  /// peer's key, reports it and moves to Progress::verifying.
  ///
  /// Stops the hello timeout and starts pairing_timeout_us.
  ///
  /// @param message The Identity.
  /// @return Nothing on success, or the ending: a protocol failure out of
  ///     order, or the verification failure.
  auto identify(const net::kakusan::control::Identity& message)
      -> std::optional<Ending>;

  /// @brief Keeps this side's code and has it shown to the user.
  /// @param event The code; it is taken.
  /// @return False, ignoring the code, when not verifying or a code is
  ///     already held.
  auto show_code(LocalCode& event) -> bool;
  /// @brief Keeps the code the user entered for the peer.
  /// @param event The code; it is taken.
  /// @return False, ignoring the code, when not verifying or a code is
  ///     already held.
  auto take_code(PeerCodeEntered& event) -> bool;
  /// @brief Tells whether both codes are held.
  /// @return True when the exchange can begin.
  auto codes_ready() const noexcept -> bool;
  /// @brief Starts the code exchange from both codes, then wipes the codes.
  ///
  /// Both codes must be held.
  ///
  /// @return The exchange in progress.
  /// @retval Ending A failure deriving the password or drawing the ephemeral
  ///     key.
  auto exchange() -> Expected<net::kakusan::Exchange, Ending>;
  /// @brief Completes the code exchange with the peer's share and keeps the
  /// confirmation MACs; the shared key itself is wiped at once.
  /// @param exchange The exchange in progress.
  /// @param peer_share The peer's share.
  /// @return Nothing on success, or the ending: a failed verification when
  ///     the share is unusable, otherwise the failure.
  auto confirm(net::kakusan::Exchange& exchange,
               std::span<const std::byte> peer_share) -> std::optional<Ending>;
  /// @brief Checks the peer's confirmation MAC in constant time. confirm()
  /// must have succeeded.
  /// @param mac The MAC the peer sent.
  /// @return True when it matches, meaning both sides used the same codes.
  auto peer_confirmed(const net::kakusan::Digest512& mac) const noexcept
      -> bool;
  /// @brief Sends this side's confirmation MAC: ConfirmA from the host,
  /// ConfirmB from the guest.
  void send_confirmation();
  /// @brief Wipes the confirmation MACs once they have been used.
  void drop_confirmations() noexcept;
  /// @brief Checks the transcript in the peer's Authenticated.
  /// @param message The message.
  /// @return Nothing when it matches, or a transcript failure.
  auto check_authenticated(const net::kakusan::control::Authenticated& message)
      const -> std::optional<Ending>;
  /// @brief Sends Authenticated with the transcript.
  void send_authenticated();
  /// @brief Moves to Progress::waiting_for_seat after verification: stops
  /// the pairing timeout, wipes the exported keying material, consumes the
  /// invitation and starts the heartbeat.
  void admit();

  /// @brief Notes that something arrived from the peer, for the silence
  /// check.
  void refresh() noexcept;
  /// @brief Answers a Ping with a Pong.
  /// @param message The Ping.
  /// @return Nothing, or a protocol failure before verification.
  auto answer_ping(const net::kakusan::control::Ping& message)
      -> std::optional<Ending>;
  /// @brief Takes a Pong; one that answers the outstanding ping counts as
  /// contact.
  /// @param message The Pong.
  /// @return Nothing, or a protocol failure before verification.
  auto take_pong(const net::kakusan::control::Pong& message)
      -> std::optional<Ending>;
  /// @brief Sends a Ping when one is due.
  /// @return Nothing, or a failure when no nonce could be drawn.
  auto keep_alive() -> std::optional<Ending>;
  /// @brief Tells whether the peer has been silent too long while the
  /// heartbeat runs.
  /// @return True when the liveness timeout has passed.
  auto silent() const noexcept -> bool;
  /// @brief Tells whether the link, hello or pairing timeout has passed.
  /// @return True when one has.
  auto timed_out() const noexcept -> bool;

  /// @brief A failure, told to the peer when the link is up.
  /// @param error Why.
  /// @return The ending.
  auto failure(net::kakusan::Error error) const -> Ending;
  /// @brief The ending for a Failure the peer sent.
  /// @param message The Failure.
  /// @return The ending; a failed verification reported while verifying
  ///     counts towards the cooldown.
  auto peer_failed(const net::kakusan::control::Failure& message) const
      -> Ending;
  /// @brief The ending for a code exchange message that the peer should not
  /// have sent.
  /// @return A protocol failure once verified, a failed verification before.
  auto unexpected_pake() const -> Ending;
  /// @brief Handles the link failing.
  /// @return Nothing when leaving (the player is now gone) or already gone;
  ///     otherwise the ending: a network ending once the link was up, no path
  ///     before.
  auto transport_failed() -> std::optional<Ending>;
  /// @brief Handles an acknowledgement as the channel sees it.
  ///
  /// While leaving, the acknowledgement of the last message finishes the
  /// player.
  ///
  /// @param token The token acknowledged.
  /// @return True when the channel dealt with it (token zero, leaving or
  ///     gone) and the player should not look further.
  auto acknowledged(net::kakusan::AckToken token) -> bool;
  /// @brief Finishes a leaving player once terminal_grace_us has passed.
  /// @return True while leaving or gone, when the player has no other timers
  ///     to check.
  auto closing() -> bool;
  /// @brief Ends the visit.
  ///
  /// Wipes secrets, consumes the invitation if not yet done, counts a failed
  /// verification when the ending says so, and stops every timer. When the
  /// link is up and there is a last message to send or the peer ended it,
  /// moves to Progress::leaving and sends the message; otherwise goes
  /// straight to Progress::gone and closes the link.
  ///
  /// @param ending How it ends.
  void end(Ending ending);

 private:
  /// @brief The other side's role.
  /// @return Guest for the host, host for the guest.
  auto peer_role() const noexcept -> net::kakusan::Role;
  /// @brief Reports the current stage and the ending shown so far.
  void show();
  /// @brief Moves to Progress::gone and closes the link.
  void finish();
  /// @brief Clears every timer and stops the heartbeat.
  void stop_timers() noexcept;
  /// @brief Wipes the codes, the confirmations and the exported keying
  /// material.
  void forget_secrets() noexcept;

  /// @brief Crypto; never null.
  platform::CryptoProvider* crypto_;
  /// @brief Randomness; never null.
  platform::SecureRandomProvider* random_;
  /// @brief Which side this is.
  net::kakusan::Role role_;
  /// @brief What both sides share; empty on the host until a reply is
  /// imported.
  std::optional<PairingContext> context_;
  /// @brief The stage reached.
  Progress state_{Progress::waiting};
  /// @brief The latest monotonic time seen, in microseconds.
  std::uint64_t now_{};
  /// @brief The link and invitation machine, which owns the outbox.
  player_lifecycle::Channel<Effect> lifecycle_;
  /// @brief The token of the last message sent while leaving.
  net::kakusan::AckToken terminal_token_{};

  /// @brief The transcript followed by the link's exported keying material,
  /// bound into the code exchange; wiped once admitted or ending.
  SecretArray<2 * net::kakusan::pairing_exporter_size> associated_;
  /// @brief How far the introduction has got.
  Introduction introduction_{Introduction::awaiting_hello};
  /// @brief This side's Identity proof, held until the peer's Hello arrives.
  std::optional<net::kakusan::control::Identity> proof_;
  /// @brief This side's code, held until the exchange begins.
  std::optional<net::kakusan::Code> local_code_;
  /// @brief The peer's code as entered, held until the exchange begins.
  std::optional<net::kakusan::Code> peer_code_;
  /// @brief Both confirmation MACs, held until checked.
  std::optional<net::kakusan::Confirmations> confirmations_;

  /// @brief When the wait for the encrypted link times out, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> dtls_deadline_;
  /// @brief When the wait for Hello and Identity times out, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> hello_deadline_;
  /// @brief When verifying times out, in monotonic microseconds.
  std::optional<std::uint64_t> pairing_deadline_;
  /// @brief When a leaving player gives up waiting and closes, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> closing_deadline_;
  /// @brief Pings and silence detection, running once admitted.
  net::kakusan::Heartbeat heartbeat_;

  /// @brief The report sent with each ShowState: the stage, and once ending,
  /// why and by whom.
  ShowState shown_;
};

}  // namespace libchiaking::kakusan
