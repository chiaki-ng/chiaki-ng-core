/// @file
/// @brief Declares SharedSession::Core, the host's Kakusan protocol: the
/// invitations, a state machine and link per guest, the seats, and the
/// forwarding of the console's stream to every guest receiving media.
#pragma once

#include "libchiaking/detail/kakusan/device_identity.hpp"
#include "libchiaking/detail/kakusan/shared_session.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/kakusan/adapters/capture.hpp"
#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/adapters/effect_loop.hpp"
#include "src/core/kakusan/adapters/invitation_book.hpp"
#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/kakusan/adapters/shared_session/seat_table.hpp"
#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/kakusan/model/clock.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/link.hpp"
#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/short_block.hpp"

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

/// @brief The protocol side of one shared session, with no frontend or console
/// in it.
///
/// For each invitation the core keeps a spoke: the guest's id and invitation
/// seed, its `net::kakusan` link, its InvitedPlayer and the media sender that
/// forwards the stream once the guest plays. Commands and console happenings
/// come in through the members below, each stamped with the time; what the
/// frontend and the console must be told is queued as Output and taken with
/// take_output(). pump() moves the links, the machines' timers, invitation
/// expiry and media forward. Spokes whose visit is gone are dropped after each
/// step.
///
/// The stream is forwarded encrypted as the console sent it: the core keeps
/// the receive keys from the host's session, sends them to each guest in the
/// source setup, and forwards datagrams taken from its Capture. A new set of
/// keys ends the visits of guests whose setup was built from the old ones.
///
/// Owned by SharedSession::Hosting, made only by create(). Not thread-safe:
/// every member must be called with the session's lock held, except
/// source_keys() and source_datagram(), which go straight to the Capture and
/// may be called from the host's stream receive path. Neither copyable nor
/// movable; the destructor closes every link.
class SharedSession::Core {
 public:
  /// @brief The shortest time between keyframe requests to the console unless
  /// tuned, in microseconds: one second.
  static constexpr std::uint64_t default_keyframe_interval_us =
      microseconds_per_second;

  /// @brief How the stream is forwarded to guests.
  struct Tuning {
    /// @brief The media senders' queue limits, ages, grace and failure
    /// rules; applies to senders started afterwards.
    net::kakusan::MediaTuning media;
    /// @brief The shortest time between keyframe requests, in microseconds;
    /// applies at once.
    std::uint64_t keyframe_interval_us{default_keyframe_interval_us};
  };

  /// @brief What the core borrows; each must outlive it.
  struct Dependencies {
    /// @brief Crypto for invitations, proofs, codes and media.
    platform::CryptoProvider& crypto;
    /// @brief Randomness for ids, seeds, codes and session ids.
    platform::SecureRandomProvider& random;
    /// @brief Opens the link for each invitation.
    net::kakusan::SpokeTransportFactory& transports;
    /// @brief Makes the lock of the stream capture.
    platform::ThreadingProvider& threading;
  };

  /// @brief An invitation has gathered its network addresses and is ready to
  /// give to the guest.
  struct InvitationReady {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The invitation text.
    SecretString block;
    /// @brief When it expires, in wall-clock seconds since the epoch.
    std::uint64_t expires_at{};
    /// @brief The short hint of the host's identity key it carries.
    IdentityHint host_hint{};
    /// @brief The seat counts when it was issued.
    SeatSummary seats{};
    /// @brief The stream protocol version being shared.
    std::uint8_t source_protocol{};
  };
  /// @brief A guest proved their device identity.
  struct GuestIdentified {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The guest device's identity key.
    net::kakusan::IdentityKey identity{};
    /// @brief The SHA-256 of the key, for showing.
    net::kakusan::Digest256 fingerprint{};
  };
  /// @brief The host's verification code for a guest is ready to read out.
  struct CodeReady {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The code.
    net::kakusan::Code code;
  };
  /// @brief A guest moved to a new stage, or their visit ended.
  struct GuestState {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The stage reached.
    Progress state{Progress::waiting};
    /// @brief Why the visit failed, once it has.
    std::optional<net::kakusan::Error> error;
    /// @brief Why the visit ended, once an ending was announced.
    std::optional<net::kakusan::control::Reason> reason;
    /// @brief True when the guest's side ended or failed it.
    bool by_peer{};
    /// @brief The guest's Kakusan seat, once reserved.
    std::optional<net::kakusan::SeatBinding> seat;
  };
  /// @brief The console should seat a guest.
  struct JoinRequested {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The Kakusan seat the guest holds; report the result with
    /// source_joined() or source_join_failed() for it.
    net::kakusan::SeatBinding seat;
  };
  /// @brief The console should drop a guest's seat.
  struct RemovalRequested {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The Kakusan seat being given up.
    net::kakusan::SeatBinding seat;
    /// @brief The console seat, when the console had seated the guest.
    std::optional<controllers::SeatNumber> console_seat;
  };
  /// @brief Controller input from a seated guest for the console.
  struct ControllerInput {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The state to send for the guest's console seat.
    controllers::ControllerState state;
  };
  /// @brief The console should send a keyframe; already limited to one per
  /// keyframe interval.
  struct IdrRequested {};
  /// @brief The seat counts changed.
  struct SeatsChanged {
    /// @brief The counts now.
    SeatSummary seats;
  };
  /// @brief A line for the log about one guest, such as a media sender's
  /// report.
  struct Diagnostic {
    /// @brief The guest.
    net::kakusan::InvitationId guest{};
    /// @brief The text.
    std::string text;
  };

  /// @brief Everything the core asks of the host's frontend and console.
  using Output =
      std::variant<InvitationReady, GuestIdentified, CodeReady, GuestState,
                   JoinRequested, RemovalRequested, ControllerInput,
                   IdrRequested, SeatsChanged, Diagnostic>;

  /// @brief Builds the core with its seat table and a random session id.
  /// @param dependencies What it borrows.
  /// @param identity The host device's identity, which signs its proofs.
  /// @param rules The seat counts and allowed buttons.
  /// @return The core.
  /// @retval net::kakusan::Error::internal The capacity or local seat count
  ///     is zero.
  /// @retval net::kakusan::Error::no_capacity There are more local seats than
  ///     seats.
  /// @retval net::kakusan::Error::crypto_unavailable No random session id
  ///     could be drawn.
  [[nodiscard]] static auto create(Dependencies dependencies,
                                   DeviceIdentity identity,
                                   const SharingRules& rules)
      -> net::kakusan::Result<std::unique_ptr<Core>>;
  /// @brief Closes every guest's link.
  ~Core();
  /// @brief Not copyable.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable.
  Core& operator=(const Core&) = delete;
  /// @brief Not movable; links and machines refer to it.
  Core(Core&&) = delete;
  /// @brief Not move-assignable.
  Core& operator=(Core&&) = delete;

  /// @brief Takes the stream's new receive keys into the Capture. Safe
  /// without the session's lock.
  /// @param protocol_version The stream protocol version.
  /// @param keys The keys.
  void source_keys(std::uint32_t protocol_version,
                   play::ReceiveKeys keys) noexcept;
  /// @brief Copies one encrypted stream datagram into the Capture, when
  /// forwarding. Safe without the session's lock; never blocks beyond the
  /// Capture's own lock.
  /// @param datagram The datagram.
  void source_datagram(const play::SharedDatagram& datagram) noexcept;

  /// @brief Records that the stream can be shared, or that its description or
  /// keys changed.
  ///
  /// Checks that a source setup can be built from it. When the description,
  /// frame rate and key generation are the same as before, nothing changes.
  /// Otherwise guests set up for the old stream are ended (every guest when
  /// the protocol version changed) and invitations can be issued for the new
  /// one.
  ///
  /// @param description The stream's video profiles and audio header.
  /// @param video_fps The video frame rate.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing on success.
  /// @retval net::kakusan::Error::source_format No keys have been received.
  /// @retval net::kakusan::Error::crypto_unavailable No source id could be
  ///     drawn.
  /// @retval net::kakusan::Error Any reason SharedSetup::build refused it.
  [[nodiscard]] auto source_started(const media::StreamDescription& description,
                                    std::uint32_t video_fps,
                                    std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Records that the shared stream ended: every guest's visit ends
  /// and the Capture is reset.
  /// @param now_us The current monotonic time, in microseconds.
  void source_ended(std::uint64_t now_us);
  /// @brief Records that the console seated a guest, and sends them the
  /// source setup.
  /// @param seat The guest's Kakusan seat, from JoinRequested.
  /// @param console_seat The console seat they were given.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once the setup is on its way.
  /// @retval net::kakusan::Error::source_join_failed The guest had gone while
  ///     being seated, and a RemovalRequested asks for the console seat back;
  ///     or another guest already holds that console seat, and the joining
  ///     guest's visit fails as source_join_failed() would fail it, with no
  ///     removal of the console seat, which stays with the guest holding it.
  /// @retval net::kakusan::Error::internal No guest is being seated in that
  ///     Kakusan seat.
  /// @retval net::kakusan::Error::source_ended The stream is gone or its keys
  ///     changed; the guest's visit fails.
  /// @retval net::kakusan::Error The setup could not be built; the guest's
  ///     visit fails.
  [[nodiscard]] auto source_joined(const net::kakusan::SeatBinding& seat,
                                   controllers::SeatNumber console_seat,
                                   std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Records that the console would not seat a guest; their visit
  /// fails.
  /// @param seat The guest's Kakusan seat, from JoinRequested.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing when a guest, present or gone, was waiting on that seat.
  /// @retval net::kakusan::Error::internal None was.
  [[nodiscard]] auto source_join_failed(const net::kakusan::SeatBinding& seat,
                                        std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Passes a rumble to the guest in its console seat, if any.
  /// @param rumble The rumble, already scaled.
  /// @param now_us The current monotonic time, in microseconds.
  void source_rumble(const SourceRumble& rumble, std::uint64_t now_us);
  /// @brief Records that the console dropped a seat; the guest in it, if any,
  /// has their visit ended.
  /// @param console_seat The console seat.
  /// @param now_us The current monotonic time, in microseconds.
  void source_unseated(controllers::SeatNumber console_seat,
                       std::uint64_t now_us);

  /// @brief Starts an invitation: draws its seed and id, opens a link and
  /// starts gathering addresses. InvitationReady follows once gathered, or
  /// the spoke fails with no path after the gathering timeout.
  /// @param clock The current time.
  /// @return The new guest's id.
  /// @retval net::kakusan::Error::source_ended The stream is not being
  ///     shared.
  /// @retval net::kakusan::Error::no_capacity max_tracked_invitations
  ///     invitations are issued or gathering.
  /// @retval net::kakusan::Error::crypto_unavailable No random seed could be
  ///     drawn.
  /// @retval net::kakusan::Error::internal No unused id was drawn in
  ///     id_draw_attempts tries.
  /// @retval net::kakusan::Error Opening the link or gathering failed.
  [[nodiscard]] auto invite(Clock clock)
      -> net::kakusan::Result<net::kakusan::InvitationId>;
  /// @brief Accepts a guest's reply text and starts connecting to them.
  ///
  /// Importing the same reply again returns the same id and changes nothing.
  ///
  /// @param block The reply text.
  /// @param clock The current time.
  /// @return The id of the guest the reply answers.
  /// @retval net::kakusan::Error The text is not a valid reply, or the
  ///     transcript could not be hashed, in which case the guest's visit
  ///     fails.
  /// @retval net::kakusan::Error::invalid_block It answers no open invitation
  ///     of this session, or not as InvitationBook::import_reply accepts.
  /// @retval net::kakusan::Error::expired The invitation has expired.
  /// @retval net::kakusan::Error::pairing_failed A verification cooldown is
  ///     in force.
  /// @retval net::kakusan::Error::no_capacity Too many guests are connecting.
  [[nodiscard]] auto import_reply(std::string_view block, Clock clock)
      -> net::kakusan::Result<net::kakusan::InvitationId>;
  /// @brief Takes the code a guest read out.
  /// @param guest The guest.
  /// @param code The code as entered.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once taken.
  /// @retval net::kakusan::Error::internal The guest is unknown or not
  ///     verifying.
  /// @retval net::kakusan::Error::pairing_failed A verification cooldown is
  ///     in force.
  /// @retval net::kakusan::Error The code is not four digits.
  [[nodiscard]] auto enter_code(const net::kakusan::InvitationId& guest,
                                std::string_view code, std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Approves a guest for a seat; they are seated once verified.
  /// @param guest The guest.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once recorded.
  /// @retval net::kakusan::Error::internal The guest is unknown.
  [[nodiscard]] auto approve(const net::kakusan::InvitationId& guest,
                             std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Ends a guest's visit because the host removed them.
  /// @param guest The guest.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once the ending has begun.
  /// @retval net::kakusan::Error::internal The guest is unknown.
  [[nodiscard]] auto remove(const net::kakusan::InvitationId& guest,
                            std::uint64_t now_us) -> net::kakusan::Result<void>;
  /// @brief Changes how many seats the host keeps; guests whose seats are
  /// taken back have them revoked.
  /// @param local_seats The new count.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing on success.
  /// @retval net::kakusan::Error As SeatTable::set_local_seats.
  [[nodiscard]] auto set_local_seats(std::uint8_t local_seats,
                                     std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Changes the number of seats; guests whose seats are taken back
  /// have them revoked.
  /// @param capacity The new count.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing on success.
  /// @retval net::kakusan::Error As SeatTable::set_capacity.
  [[nodiscard]] auto set_capacity(std::uint8_t capacity, std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Replaces the tuning, raising each value to its minimum and the
  /// keyframe interval to shortest_keyframe_interval_us.
  /// @param tuning The new tuning.
  void set_tuning(const Tuning& tuning) noexcept;
  /// @brief The tuning in force.
  /// @return The tuning.
  auto tuning() const noexcept -> const Tuning& { return tuning_; }
  /// @brief The wire buttons guests may press, fixed when sharing began.
  /// @return The mask.
  auto allowed_buttons() const noexcept -> std::uint32_t {
    return allowed_buttons_;
  }

  /// @brief Does the periodic work: pumps every link and hands its events to
  /// the guest's machine, runs the machines' timers, expires invitations,
  /// forwards captured datagrams to every media sender and sends what they
  /// release.
  /// @param clock The current time.
  void pump(Clock clock);
  /// @brief The earliest time pump() has work to do.
  /// @return The earliest timer of any link, machine, media sender,
  ///     invitation or pending keyframe request, in monotonic microseconds;
  ///     nothing when none is set.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Takes the oldest queued output.
  /// @return The output, or nothing when the queue is empty.
  [[nodiscard]] auto take_output() -> std::optional<Output>;
  /// @brief The seat counts.
  /// @return The counts now.
  auto seats() const noexcept -> SeatSummary;

 private:
  /// @brief One invited guest: id, seed, link, machine, timings and media
  /// sender. Defined in the source file.
  struct Spoke;

  /// @brief The stream being shared, as the guests' setups are built from it.
  struct SourceState {
    /// @brief Its video profiles and audio header.
    media::StreamDescription description;
    /// @brief Its frame rate.
    std::uint32_t video_fps{};
    /// @brief Its stream protocol version.
    std::uint32_t protocol_version{};
    /// @brief The Capture key generation the setups are built from.
    std::uint64_t generation{};
    /// @brief The random id of this source, sent in every setup.
    SecretArray<std::tuple_size_v<net::kakusan::control::SourceSessionId>>
        source_session_id;

    /// @brief The source id as a view.
    /// @return A view valid while this state lives.
    auto session_id() const noexcept -> SharedSetup::SessionId {
      return SharedSetup::SessionId{source_session_id.data(),
                                    source_session_id.size()};
    }
  };

  /// @brief A guest whose visit ended while the console was still seating
  /// them, remembered so the console's late answer can be undone.
  struct AbandonedJoin {
    /// @brief The guest who had gone.
    net::kakusan::InvitationId guest{};
    /// @brief The Kakusan seat the console was asked to fill.
    net::kakusan::SeatBinding seat;
  };

  /// @brief The lowest keyframe interval tuning may set, in microseconds.
  static constexpr std::uint64_t shortest_keyframe_interval_us = 50'000;
  /// @brief Most abandoned joins remembered; the oldest are forgotten.
  static constexpr std::size_t abandoned_join_limit = 16;
  /// @brief How many times invite() draws an id before giving up.
  static constexpr std::size_t id_draw_attempts = 8;

  /// @brief Stores the parts; see create().
  /// @param dependencies What it borrows.
  /// @param identity The host device's identity.
  /// @param rules The allowed buttons.
  /// @param seats The seat table built from the rules.
  /// @param session_id The random id of this shared session.
  Core(Dependencies dependencies, DeviceIdentity identity,
       const SharingRules& rules, SeatTable seats,
       const net::kakusan::SessionId& session_id);

  /// @brief Moves the monotonic clock forward; earlier readings are ignored.
  /// @param now_us The current monotonic time, in microseconds.
  void advance(std::uint64_t now_us) noexcept;
  /// @brief Moves the monotonic clock forward and takes the wall clock.
  /// @param clock The current time.
  void advance(Clock clock) noexcept;
  /// @brief Finds a guest's spoke.
  /// @param guest The guest.
  /// @return The spoke, or null.
  auto find(const net::kakusan::InvitationId& guest) noexcept -> Spoke*;
  /// @brief Finds the spoke waiting for the console to seat it in a shared
  /// play seat.
  /// @param seat The Kakusan seat.
  /// @return The spoke, or null.
  auto find_joining(const net::kakusan::SeatBinding& seat) noexcept -> Spoke*;
  /// @brief Finds the spoke seated in a console seat.
  /// @param seat The console seat.
  /// @return The spoke, or null.
  auto find_console_seat(controllers::SeatNumber seat) noexcept -> Spoke*;
  /// @brief Finds the spoke whose machine holds a Kakusan seat.
  /// @param seat The Kakusan seat.
  /// @return The spoke, or null.
  auto find_binding(const net::kakusan::SeatBinding& seat) noexcept -> Spoke*;
  /// @brief Finds the first spoke a predicate accepts.
  /// @tparam Matches Callable taking `const Spoke&` and returning bool.
  /// @param matches The predicate.
  /// @return The spoke, or null.
  template <class Matches>
  auto find_if(Matches matches) noexcept -> Spoke*;
  [[nodiscard]] auto
  /// @brief Tells each guest whose seat was taken back.
  /// @param revoked The seat table's answer.
  /// @return Nothing, or the seat table's failure.
  revoke_seats(
      net::kakusan::Result<std::vector<net::kakusan::SeatBinding>> revoked)
      -> net::kakusan::Result<void>;

  /// @brief Feeds an event to a guest's machine and applies the effects.
  /// @param spoke The guest.
  /// @param event The event.
  void feed(Spoke& spoke, InvitedPlayer::Event event);
  /// @brief Carries out one of a machine's effects; link and verification
  /// effects here, the rest passed on.
  /// @param spoke The guest.
  /// @param effect The effect.
  void apply(Spoke& spoke, InvitedPlayer::Effect& effect);
  /// @brief Carries out a seating effect: reserving, granting or releasing a
  /// seat, or asking the console to seat or drop the guest.
  /// @param spoke The guest.
  /// @param effect The effect.
  void apply_admission(Spoke& spoke, InvitedPlayer::Effect& effect);
  /// @brief Carries out a playing effect: media, input, neutral state,
  /// feedback or a keyframe request.
  /// @param spoke The guest.
  /// @param effect The effect.
  void apply_gameplay(Spoke& spoke, InvitedPlayer::Effect& effect);
  /// @brief Reports a guest's stage, and draws the host's code when they
  /// reach Progress::verifying.
  /// @param spoke The guest.
  /// @param shown The stage.
  void show(Spoke& spoke, const ShowState& shown);
  /// @brief Reports a guest's proved identity with its fingerprint.
  /// @param spoke The guest.
  /// @param peer The identity.
  void identify(Spoke& spoke, const PeerIdentified& peer);
  /// @brief Signs the host's identity proof, with the session id and seat
  /// counts, once a guest's link is up, and tells the machine.
  /// @param spoke The guest.
  /// @param exporter The link's exported keying material.
  void establish(Spoke& spoke, net::kakusan::Exporter exporter);
  /// @brief Turns a link's events into machine events; issues the invitation
  /// once addresses are gathered.
  /// @param spoke The guest.
  /// @param events The link's events.
  void deliver(Spoke& spoke, std::span<net::kakusan::LinkEvent> events);
  /// @brief Passes a media acknowledgement to the guest's sender.
  /// @param spoke The guest.
  /// @param record The acknowledgement record.
  void acknowledge_media(Spoke& spoke, std::span<const std::byte> record);
  /// @brief Seals the invitation with the gathered addresses, records it and
  /// reports InvitationReady.
  /// @param spoke The guest.
  /// @param local This side's addresses and certificate digest.
  void issue_invitation(Spoke& spoke,
                        const net::kakusan::TransportDescription& local);
  /// @brief Uses up a guest's invitation: finished when a reply was bound,
  /// failed when it was still offered.
  /// @param guest The guest.
  void consume_invitation(const net::kakusan::InvitationId& guest);
  /// @brief Asks the console to drop a guest's seat, remembering a join still
  /// in progress as abandoned.
  /// @param spoke The guest.
  /// @param seat The Kakusan seat.
  void remove_source(Spoke& spoke, const net::kakusan::SeatBinding& seat);
  /// @brief Settles the console's late answer for an abandoned join, asking
  /// for the console seat back when it was given.
  /// @param seat The Kakusan seat.
  /// @param console_seat The console seat given, or nothing when refused.
  /// @return True when the seat was an abandoned join.
  [[nodiscard]] auto resolve_abandoned(
      const net::kakusan::SeatBinding& seat,
      std::optional<controllers::SeatNumber> console_seat) -> bool;
  /// @brief Starts a media sender for a guest with the link's media key,
  /// turns forwarding on and asks for a keyframe.
  /// @param spoke The guest.
  void start_media(Spoke& spoke);
  /// @brief Reports and drops a guest's media sender.
  /// @param spoke The guest.
  void stop_media(Spoke& spoke);
  /// @brief Turns capture forwarding on while any guest has a media sender.
  void update_forwarding();
  /// @brief Notes that a keyframe is wanted; settle() sends the request.
  void request_idr() noexcept;

  /// @brief Expires invitations: fails a spoke that could not gather in time,
  /// and expires one whose lifetime or wall-clock expiry has passed.
  void expire_invitations();
  /// @brief Ends guests set up from keys that are no longer current, and
  /// forgets the stream until source_started() is called again.
  void check_source_keys();
  /// @brief Ends the visits of guests who were sent a setup, or of every
  /// guest.
  /// @param every_spoke True to end every guest.
  void end_source_spokes(bool every_spoke);
  /// @brief Hands captured datagrams that pass PacketVerdict to every media
  /// sender, asking for a keyframe when video was dropped.
  void forward_capture();
  /// @brief Sends what each media sender releases, and ends a guest whose
  /// sender failed.
  void poll_senders();
  /// @brief Drops gone spokes, updates forwarding, sends a due keyframe
  /// request and reports changed seat counts.
  void settle();
  /// @brief Writes a guest's media sender report as a Diagnostic, once.
  /// @param spoke The guest.
  void report_media(Spoke& spoke);
  /// @brief Queues an output.
  /// @param output The output.
  void emit(Output output);

  /// @brief Crypto.
  platform::CryptoProvider& crypto_;
  /// @brief Randomness.
  platform::SecureRandomProvider& random_;
  /// @brief Opens links.
  net::kakusan::SpokeTransportFactory& transports_;
  /// @brief The host device's identity.
  DeviceIdentity identity_;
  /// @brief The short hint of the host's key, put in every invitation.
  net::kakusan::IdentityHint identity_hint_;
  /// @brief The wire buttons guests may press.
  std::uint32_t allowed_buttons_;
  /// @brief The tuning in force.
  Tuning tuning_;
  /// @brief The seats.
  SeatTable seats_;
  /// @brief The invitations and the verification cooldown.
  InvitationBook book_;
  /// @brief The random id of this shared session, in the host's proofs.
  net::kakusan::SessionId session_id_{};
  /// @brief The stream's keys and the queue of datagrams to forward.
  Capture capture_;
  /// @brief The stream being shared; empty before it starts or after it ends.
  std::optional<SourceState> source_;
  /// @brief The guests, in invitation order.
  std::vector<std::unique_ptr<Spoke>> spokes_;
  /// @brief Joins abandoned while the console was seating, oldest first.
  std::deque<AbandonedJoin> abandoned_;
  /// @brief Outputs waiting for take_output().
  std::deque<Output> outputs_;
  /// @brief The seat counts last reported.
  SeatSummary reported_seats_{};
  /// @brief Scratch space for the datagrams a sender releases.
  std::vector<std::vector<std::byte>> media_;
  /// @brief When a keyframe was last requested, in monotonic microseconds.
  std::optional<std::uint64_t> last_idr_us_;
  /// @brief The latest monotonic time seen, in microseconds.
  std::uint64_t now_{};
  /// @brief The latest wall-clock time seen, in seconds since the epoch.
  std::uint64_t wall_{};
  /// @brief Whether a keyframe is wanted but not yet requested.
  bool idr_pending_{};
  /// @brief Whether the Capture is keeping datagrams.
  bool forwarding_{};
};

}  // namespace libchiaking::kakusan
