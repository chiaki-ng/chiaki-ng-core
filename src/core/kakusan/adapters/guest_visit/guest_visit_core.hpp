/// @file
/// @brief Declares GuestVisit::Core, the guest's Kakusan protocol: the
/// accepted invitation, the link to the host, the visit's state machine, and
/// the forwarded stream it receives.
#pragma once

#include "libchiaking/detail/kakusan/device_identity.hpp"
#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/kakusan/adapters/effect_loop.hpp"
#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"
#include "src/core/kakusan/adapters/invitation_book.hpp"
#include "src/core/kakusan/model/clock.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/link.hpp"
#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/short_block.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::kakusan {

/// @brief The protocol side of one guest visit, with no frontend in it.
///
/// accept_invitation() opens a link and starts gathering this device's
/// addresses. Once gathered, the reply is sealed (ReplyReady) and the link
/// connects to the host; the VisitingPlayer is made at that point. From then
/// on link events are fed to the machine and its effects carried out: codes
/// shown, the forwarded stream opened from the source setup, media received
/// and handed to it, input sent, rumble reported. What the frontend must be
/// told is queued as Output and taken with take_output().
///
/// The forwarded stream is a play::ForwardedStream built on the surroundings'
/// stream platform. It is fed the packets the media receiver recovers, still
/// encrypted with the console's keys, and decodes them for the frontend's
/// MediaSink.
///
/// Owned by GuestVisit::Visiting, made only by create(). Not thread-safe:
/// apart from the static preview(), every member must be called with the
/// visit's lock held. Neither copyable nor movable; the destructor closes the
/// link.
class GuestVisit::Core {
 public:
  /// @brief What the core borrows; each must outlive it.
  struct Dependencies {
    /// @brief Crypto, clock and log for the forwarded stream; its crypto
    /// also serves the protocol.
    platform::PlatformFactory& platform;
    /// @brief Randomness for the reply seed, codes and pings.
    platform::SecureRandomProvider& random;
    /// @brief Opens the link to the host.
    net::kakusan::SpokeTransportFactory& transports;
    /// @brief Receives the forwarded stream's video and audio.
    media::MediaSink& media;
  };

  /// @brief The reply text is ready to send back to the host.
  struct ReplyReady {
    /// @brief The reply text.
    SecretString block;
  };
  /// @brief The guest's verification code is ready to read out.
  struct CodeReady {
    /// @brief The code.
    net::kakusan::Code code;
  };
  /// @brief The host proved its device identity.
  struct HostIdentified {
    /// @brief The host device's identity key.
    net::kakusan::IdentityKey key{};
    /// @brief The SHA-256 of the key, for showing.
    net::kakusan::Digest256 fingerprint{};
    /// @brief The seat counts from the host's proof, with the capacity from
    /// the invitation.
    SeatSummary seats{};
  };
  /// @brief The visit moved to a new stage, or ended.
  struct State {
    /// @brief The stage reached.
    Progress state{Progress::waiting};
    /// @brief Why the visit failed, once it has.
    std::optional<net::kakusan::Error> error;
    /// @brief Why the visit ended, once an ending was announced.
    std::optional<net::kakusan::control::Reason> reason;
    /// @brief True when the host ended or failed it.
    bool by_peer{};
    /// @brief The seat granted, once there is one.
    std::optional<net::kakusan::SeatBinding> seat;
  };
  /// @brief The forwarded stream opened.
  struct StreamReady {
    /// @brief Its video frame rate.
    std::uint32_t video_fps{};
  };
  /// @brief The host granted a seat; the guest's controller should be readied
  /// with controller_ready().
  struct ControllerRequested {
    /// @brief The granted seat.
    net::kakusan::SeatBinding seat;
    /// @brief The total number of seats.
    std::uint8_t capacity{};
    /// @brief How many the host keeps.
    std::uint8_t local_seats{};
    /// @brief The wire buttons the guest may press.
    std::uint32_t allowed_buttons{};
  };
  /// @brief The guest's rumble changed; both zero to stop it.
  struct Rumble {
    /// @brief Low-frequency motor strength, 0 to 65535.
    std::uint16_t low_frequency{};
    /// @brief High-frequency motor strength, 0 to 65535.
    std::uint16_t high_frequency{};
  };
  /// @brief The forwarded stream, announced earlier with StreamReady, closed.
  struct StreamStopped {};
  /// @brief A line for the log, such as the media receiver's report.
  struct Diagnostic {
    /// @brief The text.
    std::string text;
  };

  /// @brief Everything the core asks of the guest's frontend.
  using Output =
      std::variant<ReplyReady, HostIdentified, CodeReady, State, StreamReady,
                   ControllerRequested, Rumble, StreamStopped, Diagnostic>;

  /// @brief Reads an invitation without accepting it, as Kakusan::read
  /// does.
  /// @param crypto Crypto to open the text with.
  /// @param block The invitation text.
  /// @param wall_seconds The current wall-clock time, in seconds since the
  ///     epoch.
  /// @return What the invitation offers, without recognition.
  /// @retval net::kakusan::Error The text is not a valid invitation.
  /// @retval net::kakusan::Error::expired It has expired.
  /// @retval net::kakusan::Error::invalid_block Its expiry is implausibly far
  ///     ahead.
  [[nodiscard]] static auto preview(platform::CryptoProvider& crypto,
                                    std::string_view block,
                                    std::uint64_t wall_seconds)
      -> net::kakusan::Result<InvitationSummary>;
  /// @brief Builds an idle core.
  /// @param dependencies What it borrows.
  /// @param identity The guest device's identity, which signs its proof.
  /// @return The core.
  /// @retval net::kakusan::Error::crypto_unavailable The platform has no
  ///     crypto provider.
  [[nodiscard]] static auto create(Dependencies dependencies,
                                   DeviceIdentity identity)
      -> net::kakusan::Result<std::unique_ptr<Core>>;
  /// @brief Drops the machine and closes the link.
  ~Core();
  /// @brief Not copyable.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable.
  Core& operator=(const Core&) = delete;
  /// @brief Not movable; the link and machine refer to it.
  Core(Core&&) = delete;
  /// @brief Not move-assignable.
  Core& operator=(Core&&) = delete;

  /// @brief Accepts an invitation: checks it, draws the reply seed, opens the
  /// link and starts gathering addresses. The invitation is waited on until
  /// its expiry, at most a lifetime plus clock_skew_tolerance_seconds.
  /// @param block The invitation text.
  /// @param clock The current time.
  /// @return What the invitation offers.
  /// @retval net::kakusan::Error::internal An invitation was already accepted
  ///     or the visit is closed.
  /// @retval net::kakusan::Error The text is not valid, has expired or is
  ///     implausible (see preview()), or opening the link or gathering
  ///     failed.
  /// @retval net::kakusan::Error::crypto_unavailable No seed could be drawn.
  [[nodiscard]] auto accept_invitation(std::string_view block, Clock clock)
      -> net::kakusan::Result<InvitationSummary>;
  /// @brief Takes the code the host read out, once.
  /// @param code The code as entered.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once taken.
  /// @retval net::kakusan::Error::internal Not verifying, or a code was
  ///     already entered.
  /// @retval net::kakusan::Error The code is not four digits.
  [[nodiscard]] auto enter_code(std::string_view code, std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Reports that the guest's controller is ready for the granted
  /// seat.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once reported.
  /// @retval net::kakusan::Error::internal Not getting ready, or no
  ///     controller was asked for.
  [[nodiscard]] auto controller_ready(std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Takes the guest's latest controller state; it is sent while
  /// playing.
  /// @param state The state, in wire form.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once taken.
  /// @retval net::kakusan::Error::internal There is no visit yet, or it is
  ///     ending.
  /// @retval net::kakusan::Error::protocol The state is not valid.
  [[nodiscard]] auto set_controller(const net::kakusan::InputState& state,
                                    std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Leaves the visit: tells a connected host, or gives up at once
  /// before the reply was ready.
  /// @param now_us The current monotonic time, in microseconds.
  void leave(std::uint64_t now_us);

  /// @brief Does the periodic work: pumps the link and handles its events,
  /// expires the invitation, runs the machine's timers, services the
  /// forwarded stream and sends media acknowledgements.
  /// @param clock The current time.
  void pump(Clock clock);
  /// @brief The earliest time pump() has work to do.
  /// @return The earliest timer of the link, the machine, the invitation, the
  ///     forwarded stream or the media receiver, in monotonic microseconds;
  ///     nothing when none is set or the visit is closed.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Takes the oldest queued output.
  /// @return The output, or nothing when the queue is empty.
  [[nodiscard]] auto take_output() -> std::optional<Output>;
  /// @brief The forwarded stream.
  /// @return The stream, or null while none is open.
  auto forwarded() const noexcept
      -> const std::shared_ptr<play::ForwardedStream>& {
    return source_;
  }
  /// @brief The stage the visit has reached.
  /// @return The machine's stage; Progress::waiting before it exists, gone
  ///     once closed without one.
  auto state() const noexcept -> Progress;

 private:
  /// @brief Longest the guest waits on an accepted invitation, in seconds: a
  /// lifetime plus the clock skew tolerance.
  static constexpr std::uint64_t max_waiting_seconds =
      net::kakusan::invitation_lifetime_seconds + clock_skew_tolerance_seconds;
  /// @brief The wait passed to the forwarded stream's service(); an answer
  /// this long or longer sets no timer.
  static constexpr std::chrono::milliseconds idle_service{60'000};

  /// @brief Stores the parts; see create().
  /// @param dependencies What it borrows.
  /// @param crypto The platform's crypto provider.
  /// @param identity The guest device's identity.
  Core(Dependencies dependencies, platform::CryptoProvider& crypto,
       DeviceIdentity identity) noexcept;

  [[nodiscard]] static auto
  /// @brief Parses an invitation and checks its expiry.
  /// @param crypto Crypto to open the text with.
  /// @param block The text.
  /// @param wall_seconds The current wall-clock time, in seconds.
  /// @return The invitation.
  /// @retval net::kakusan::Error As preview().
  read(platform::CryptoProvider& crypto, std::string_view block,
       std::uint64_t wall_seconds)
      -> net::kakusan::Result<net::kakusan::SealedInvitation>;
  /// @brief What an invitation offers, as the public summary.
  /// @param invitation The invitation.
  /// @return The host hint, capacity, expiry and stream protocol.
  static auto summarize(const net::kakusan::SealedInvitation& invitation)
      -> InvitationSummary;
  /// @brief The visit failure for a refusal of the forwarded stream.
  /// @param refusal The refusal.
  /// @return crypto_unavailable or source_format; nothing for a media failure
  ///     the stream records as its own fault, which the visit reports
  ///     separately.
  [[nodiscard]] auto forwarded_failure(play::ForwardedStream::Refusal refusal)
      const -> std::optional<net::kakusan::Error>;

  /// @brief Moves the monotonic clock forward; earlier readings are ignored.
  /// @param now_us The current monotonic time, in microseconds.
  void advance(std::uint64_t now_us) noexcept;
  /// @brief Tells whether an invitation is accepted and the host has not yet
  /// connected.
  /// @return True when so.
  auto waiting() const noexcept -> bool;
  /// @brief Tells whether the visit is ending or over.
  /// @return True at Progress::leaving or gone.
  auto finished() const noexcept -> bool;

  /// @brief Turns one link event into machine input; signs the guest's proof
  /// when the link comes up. A failure before the machine exists abandons the
  /// visit with no path.
  /// @param event The event.
  void deliver(net::kakusan::LinkEvent& event);
  /// @brief Seals the reply with the gathered addresses, makes the machine,
  /// connects to the host and reports ReplyReady; abandons the visit when the
  /// invitation has expired or the reply cannot be sealed.
  /// @param local This side's addresses and certificate digest; the addresses
  ///     are taken.
  void on_gathered(net::kakusan::TransportDescription& local);
  /// @brief Feeds a control message to the machine.
  /// @param message The message; it is taken.
  void on_control(net::kakusan::control::Message& message);
  /// @brief Opens a media datagram while playing and hands its packet to the
  /// forwarded stream.
  /// @param datagram The datagram.
  void on_media(std::span<const std::byte> datagram);
  /// @brief Gives up waiting once the invitation's wall-clock expiry or the
  /// monotonic wait has passed.
  void expire_invitation();
  /// @brief Services the forwarded stream and sends the media receiver's
  /// acknowledgements.
  void poll_receivers();
  /// @brief Asks the host for a keyframe when the forwarded stream wants one.
  void request_idr();

  /// @brief Feeds an event to the machine, if it exists, and applies the
  /// effects.
  /// @param event The event.
  void feed(VisitingPlayer::Event event);
  /// @brief Applies effects the machine already produced.
  /// @param effects The effects.
  void run(VisitingPlayer::Effects effects);
  /// @brief Carries out one effect through its on() overload.
  /// @param effect The effect.
  void apply(VisitingPlayer::Effect& effect);

  /// @brief Sends a control message; a failure to send counts as the link
  /// failing.
  /// @param effect The effect.
  void on(SendControl& effect);
  /// @brief Reports the stage, and draws the guest's code on reaching
  /// verifying.
  /// @param effect The effect.
  void on(ShowState& effect);
  /// @brief Reports the host's identity with its fingerprint and seat counts;
  /// fails without the host's session details.
  /// @param effect The effect.
  void on(PeerIdentified& effect);
  /// @brief Reports the guest's code.
  /// @param effect The effect.
  void on(DisplayCode& effect);
  /// @brief Drops the media receiver and forwarded stream, and any code not
  /// yet delivered.
  /// @param effect The effect.
  void on(EraseSecrets& effect);
  /// @brief Closes the visit: stops media and closes the link.
  /// @param effect The effect.
  void on(CloseTransport& effect);
  /// @brief Does nothing: the guest keeps no cooldown.
  /// @param effect The effect.
  void on(RecordAuthenticationFailure& effect);
  /// @brief Does nothing: the guest keeps no invitation book.
  /// @param effect The effect.
  void on(ConsumeInvitation& effect);
  /// @brief Opens the forwarded stream from the setup and reports
  /// StreamReady.
  /// @param effect The effect.
  void on(ConfigureReceiver& effect);
  /// @brief Makes the media receiver from the link's media key.
  /// @param effect The effect.
  void on(StartMediaReception& effect);
  /// @brief Stops media.
  /// @param effect The effect.
  void on(StopMediaReception& effect);
  /// @brief Reports ControllerRequested.
  /// @param effect The effect.
  void on(PrepareController& effect);
  /// @brief Sends an input frame while playing.
  /// @param effect The effect.
  void on(SendInput& effect);
  /// @brief Reports the rumble.
  /// @param effect The effect.
  void on(ApplyRumble& effect);
  /// @brief Reports a zero rumble.
  /// @param effect The effect.
  void on(StopRumble& effect);

  /// @brief Draws the guest's code, once.
  void present_code();
  /// @brief Reports and drops the media receiver, closes the forwarded stream
  /// and reports StreamStopped if it was announced.
  void stop_media();
  /// @brief Gives up before the machine exists: wipes, closes the link and
  /// reports Progress::gone.
  /// @param error Why it failed, if it did.
  /// @param reason Why it ended, if it was left.
  void abandon(std::optional<net::kakusan::Error> error,
               std::optional<net::kakusan::control::Reason> reason);
  /// @brief Drops the media receiver and forwarded stream.
  void forget_secrets() noexcept;
  /// @brief Drops any CodeReady still queued.
  void discard_codes() noexcept;

  /// @brief The stream platform; never null.
  platform::PlatformFactory* platform_;
  /// @brief The platform's crypto; never null.
  platform::CryptoProvider* crypto_;
  /// @brief Randomness; never null.
  platform::SecureRandomProvider* random_;
  /// @brief Opens the link; never null.
  net::kakusan::SpokeTransportFactory* transports_;
  /// @brief Receives the stream; never null.
  media::MediaSink* sink_;
  /// @brief The guest device's identity.
  DeviceIdentity identity_;
  /// @brief The latest monotonic time seen, in microseconds.
  std::uint64_t now_{};
  /// @brief The latest wall-clock time seen, in seconds since the epoch.
  std::uint64_t wall_seconds_{};

  /// @brief The accepted invitation.
  std::optional<net::kakusan::SealedInvitation> invitation_;
  /// @brief The reply's random seed.
  net::kakusan::BlockSeed seed_{};
  /// @brief When waiting on the invitation gives up, in monotonic
  /// microseconds.
  std::uint64_t expiry_us_{};
  /// @brief The link to the host.
  std::unique_ptr<net::kakusan::Link> link_;
  /// @brief The visit's machine, from when the reply is sealed.
  std::unique_ptr<VisitingPlayer> spoke_;
  /// @brief The hash of the invitation and reply.
  net::kakusan::Transcript transcript_{};
  /// @brief Feeds the machine without reentering it.
  EffectLoop<VisitingPlayer> loop_;

  /// @brief Opens media datagrams while playing.
  std::unique_ptr<net::kakusan::MediaReceiver> media_;
  /// @brief The forwarded stream, shared with the visit for video calls.
  std::shared_ptr<play::ForwardedStream> source_;
  /// @brief When the forwarded stream next wants service, in monotonic
  /// microseconds.
  std::optional<std::uint64_t> source_deadline_us_;
  /// @brief Outputs waiting for take_output().
  std::deque<Output> outputs_;

  /// @brief Whether the guest's code was drawn.
  bool code_drawn_{};
  /// @brief Whether the host's code was entered.
  bool code_entered_{};
  /// @brief Whether a controller was asked for and not yet readied.
  bool controller_requested_{};
  /// @brief Whether StreamReady was reported, so StreamStopped is owed.
  bool stream_announced_{};
  /// @brief Whether the visit is closed.
  bool closed_{};
  /// @brief Media datagrams received, for the report.
  std::uint64_t media_datagrams_{};
  /// @brief Stream packets recovered from them, for the report.
  std::uint64_t media_packets_{};
  /// @brief Media acknowledgements sent, for the report.
  std::uint64_t media_acks_{};
  /// @brief When the first media datagram arrived, in monotonic microseconds.
  std::optional<std::uint64_t> media_first_us_;
};

}  // namespace libchiaking::kakusan
