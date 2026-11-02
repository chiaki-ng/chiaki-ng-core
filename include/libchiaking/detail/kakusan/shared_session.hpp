/// @file
/// @brief Declares kakusan::SharedSession, the host's side of a shared
/// session, and the values that describe its players.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/invitation.hpp"
#include "libchiaking/detail/kakusan/remembered_devices.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::kakusan {

class Kakusan;
class Surroundings;
class Connections;
class PumpGate;

/// @brief How the host forwards the stream to guests, for experiments and
/// diagnostics.
///
/// Passed to SharedSession::tune. Each value is raised to a built-in minimum if
/// it is below it.
///
/// @warning The members default to zero, not to the library's defaults. Passing
///     a value-initialised Tuning asks for the minimum of everything; set every
///     field.
/// @ingroup api_kakusan
struct Tuning {
  /// @brief Lower bound on the age at which a packet still waiting in a
  /// guest's send queue is dropped.
  std::chrono::milliseconds queue_age_min{};
  /// @brief Upper bound on that age; never less than queue_age_min.
  std::chrono::milliseconds queue_age_max{};
  /// @brief The age at which queued packets are dropped, as a multiple of the
  /// connection's measured round-trip time, kept between queue_age_min and
  /// queue_age_max.
  std::uint32_t queue_age_round_trips{};
  /// @brief Time after a guest's media starts during which dropped packets do
  /// not count against the guest.
  std::chrono::milliseconds startup_grace{};
  /// @brief How many consecutive seconds in which at least half of a guest's
  /// packets were shed end the visit with Refusal::Kind::connection_too_slow.
  /// Packets are shed when they wait too long in the send queue, or while the
  /// connection's round-trip time sits well above its usual floor.
  std::uint32_t drop_seconds_to_end{};
  /// @brief How long sent media may go unacknowledged before the guest's
  /// media is given up on.
  std::chrono::milliseconds acknowledgement_timeout{};
  /// @brief Most bytes held in one guest's send queue.
  std::size_t queue_bytes{};
  /// @brief Most packets held in one guest's send queue.
  std::size_t queue_packets{};
  /// @brief Shortest time between keyframe requests made to the console on
  /// behalf of guests.
  std::chrono::milliseconds keyframe_interval{};
};

/// @brief How a guest should be seated on the console, given with
/// SharedSession::give_seat.
/// @ingroup api_kakusan
struct GuestSeating {
  /// @brief The kind of controller the console should see for this guest.
  controllers::ControllerKind controller{
      controllers::ControllerKind::console_default};
  /// @brief The account to sign the guest's seat in as, when the console
  /// needs one.
  std::optional<AccountId> as{};
  /// @brief The console seat the host would like the guest to have; the
  /// console may give another.
  std::optional<controllers::SeatNumber> preferred{};
};

/// @brief A snapshot of one guest as the host sees them.
/// @ingroup api_kakusan
struct PlayerSummary {
  /// @brief The guest's id within this shared session.
  GuestId guest{};
  /// @brief The stage the guest has reached.
  Progress progress{Progress::waiting};
  /// @brief Why the guest's visit failed, when it ended in failure.
  std::optional<Refusal> refusal{};
  /// @brief Why the guest's visit ended, when an ending was announced.
  std::optional<EndReason> ended{};
  /// @brief True when the ending was started by the guest's side, false when
  /// by the host.
  bool by_them{};
  /// @brief The Kakusan seat granted to the guest, once there is one.
  std::optional<SeatGrant> grant{};
  /// @brief The console seat the guest is playing in; present once the
  /// console has seated them.
  std::optional<controllers::SeatNumber> console_seat{};
  /// @brief The guest device's public key; present once their reply has been
  /// accepted.
  std::optional<IdentityKey> device{};
  /// @brief The fingerprint of device.
  std::optional<Fingerprint> fingerprint{};
  /// @brief The console seat being held for the guest while the console is
  /// still seating them.
  std::optional<controllers::SeatNumber> reserved_seat{};
};

/// @brief A play::Session being shared with remote guests, as the host controls
/// it.
///
/// Created by Kakusan::share. For each guest the host goes through the same
/// steps:
///
/// invite_player() starts an invitation, and InvitationReady delivers the text
/// to give the guest. The guest answers with a reply text, which the host
/// passes to accept_reply(); PlayerIdentified then says which device answered.
/// Both sides receive a code (CodeToReadOut), tell each other, and enter the
/// other's with verify(). Once verified the guest is at
/// Progress::waiting_for_seat and give_seat() puts them on the console;
/// PlayerSeated confirms it and the guest starts receiving the stream and
/// sending their controller.
///
/// Commands only start work and return; results arrive as events. Events are
/// delivered to the observer while the Kakusan::Session does its regular
/// work, by whichever thread runs that session, and never from inside the
/// command that caused them. No SharedSession lock is held during delivery, so
/// an observer may call commands. Commands may be called from any thread.
///
/// Destroying the object stops sharing, as stop_sharing() does.
/// @ingroup api_kakusan
class SharedSession {
 public:
  /// @brief Internal protocol state of the shared session; not part of the
  /// public interface.
  class Core;
  /// @brief Internal runtime state of the shared session; not part of the
  /// public interface.
  class Hosting;

  /// @brief An invitation started by invite_player() is ready to give to the
  /// guest.
  struct InvitationReady {
    /// @brief The guest the invitation is for.
    GuestId guest{};
    /// @brief The invitation; deliver Invitation::text to the guest.
    Invitation invitation;
  };
  /// @brief A guest's reply was accepted and their device is now known.
  ///
  /// This is the moment to show whether the device is remembered, before any
  /// code is entered.
  struct PlayerIdentified {
    /// @brief The guest who replied.
    GuestId guest{};
    /// @brief The guest device's public key.
    IdentityKey device{};
    /// @brief The fingerprint of device.
    Fingerprint fingerprint{};
    /// @brief Whether the device is one the host remembers.
    Recognition recognition{};
  };
  /// @brief The host's verification code for a guest is ready to be read out.
  ///
  /// The host tells the guest this code and enters the guest's code with
  /// SharedSession::verify.
  struct CodeToReadOut {
    /// @brief The guest the code is for.
    GuestId guest{};
    /// @brief The four digits to read out.
    VerificationCode code;
  };
  /// @brief A guest moved to a new stage, was given or lost a seat
  /// reservation, or left.
  struct PlayerChanged {
    /// @brief The guest as they are now.
    PlayerSummary player;
  };
  /// @brief The console seated a guest; they move on to
  /// Progress::getting_ready and then Progress::playing.
  struct PlayerSeated {
    /// @brief The guest who was seated.
    GuestId guest{};
    /// @brief The console seat they hold.
    controllers::SeatNumber seat;
  };
  /// @brief A guest who was playing no longer holds a console seat.
  struct PlayerUnseated {
    /// @brief The guest who was unseated.
    GuestId guest{};
    /// @brief The console seat that was freed.
    controllers::SeatNumber seat;
  };
  /// @brief The seat counts changed.
  struct SeatsChanged {
    /// @brief The counts as they are now.
    SeatSummary seats;
  };
  /// @brief The host's stream has started being forwarded; invitations can be
  /// issued from now on.
  struct StreamShared {
    /// @brief The frame rate of the shared video.
    std::uint32_t frames_per_second{};
  };
  /// @brief The stream cannot be shared, although the session itself goes on.
  struct SharingFailed {
    /// @brief Why. Refusal::Kind::stream_not_shareable when the stream's
    /// format cannot be forwarded and Refusal::Kind::unsupported_version
    /// when its protocol version cannot are the usual causes.
    Refusal refusal{Refusal::Kind::internal};
  };
  /// @brief The Kakusan::Session ended, and sharing with it. Not sent
  /// after stop_sharing().
  struct SharingEnded {};

  /// @brief Everything a SharedSession can report to its observer.
  using Event =
      std::variant<InvitationReady, PlayerIdentified, CodeToReadOut,
                   PlayerChanged, PlayerSeated, PlayerUnseated, SeatsChanged,
                   StreamShared, SharingFailed, SharingEnded>;

  /// @brief Receives the events of one shared session; implemented by the
  /// frontend.
  class Observer {
   public:
    /// @brief Destroys the observer through its interface.
    virtual ~Observer() = default;
    /// @brief Receives one event.
    ///
    /// Called by the thread that runs the play::Session being shared, with
    /// no SharedSession lock held, so the handler may call SharedSession
    /// commands. It must not destroy the SharedSession.
    ///
    /// @param event The event; std::visit over it to handle each kind.
    virtual void on_event(Event event) = 0;
  };

  /// @brief Timing and limits of a shared session.
  struct Pacing {
    /// @brief Longest the shared session's loop may sleep between turns
    /// while any guest is admitted, so media keeps flowing.
    std::chrono::milliseconds admitted_wait{2};
    /// @brief Most guests whose state is tracked for players() at once.
    std::size_t tracked_players{16};
  };

  /// @brief Longest diagnostic note, in characters, that a shared session
  /// writes to the log; longer notes are dropped.
  static constexpr std::size_t diagnostic_limit = 512;

  /// @brief Wraps the internal state of a shared session. Frontends obtain
  /// one from Kakusan::share instead.
  /// @param hosting The assembled session.
  explicit SharedSession(std::unique_ptr<Hosting> hosting) noexcept;
  /// @brief Stops sharing and releases every guest connection.
  ///
  /// Blocks until any event delivery in progress on the session's thread
  /// has finished, so it must not run inside Observer::on_event.
  ~SharedSession();
  /// @brief Not copyable; a shared session has one owner.
  SharedSession(const SharedSession&) = delete;
  /// @brief Not copy-assignable; a shared session has one owner.
  auto operator=(const SharedSession&) -> SharedSession& = delete;

  /// @brief Starts an invitation for one more guest.
  ///
  /// The invitation text is not ready when this returns: it arrives in
  /// InvitationReady once this device has gathered its network addresses. An
  /// invitation lasts ten minutes and admits one guest.
  ///
  /// @return The id the new guest is known by from now on.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped, or the stream
  ///     has not started being shared yet (wait for StreamShared).
  /// @retval Refusal::Kind::no_free_seat Too many invitations are
  ///     outstanding. This does not look at the seat counts.
  /// @retval Refusal::Kind::internal A random id could not be drawn.
  /// @note Other kinds can come back when the network connection for the
  ///     guest could not be prepared.
  [[nodiscard]] auto invite_player() -> Expected<GuestId, Refusal>;
  /// @brief Accepts the reply text a guest sent back and starts connecting to
  /// them.
  ///
  /// PlayerIdentified follows, then PlayerChanged as the connection and
  /// verification progress. Passing the same reply again is harmless and
  /// returns the same id.
  ///
  /// @param pasted The reply text exactly as the guest produced it.
  /// @return The id of the guest the reply belongs to.
  /// @retval Refusal::Kind::invalid_argument The text is empty or longer than
  ///     Invitation::most_text.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  /// @retval Refusal::Kind::invalid_text The text is not a valid reply, or
  ///     answers no open invitation of this session.
  /// @retval Refusal::Kind::expired The invitation it answers has expired.
  /// @retval Refusal::Kind::verification_failed Too many verification
  ///     attempts failed recently; replies are refused for a while.
  /// @retval Refusal::Kind::no_free_seat Too many guests are already part
  ///     way through connecting.
  /// @retval Refusal::Kind::internal The exchange could not be hashed; the
  ///     guest's visit fails as well.
  [[nodiscard]] auto accept_reply(std::string_view pasted)
      -> Expected<GuestId, Refusal>;
  /// @brief Enters the verification code a guest read out.
  ///
  /// The two sides then prove to each other that the codes match. The result
  /// arrives as PlayerChanged: Progress::waiting_for_seat on success, or
  /// Progress::gone with a refusal.
  ///
  /// @param guest The guest whose code this is.
  /// @param theirs The guest's code.
  /// @return Nothing once the code is accepted for checking.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  /// @retval Refusal::Kind::internal The guest is unknown or is not at
  ///     Progress::verifying.
  /// @retval Refusal::Kind::verification_failed Too many verification
  ///     attempts failed recently and codes are refused for a while, or the
  ///     code is not four ASCII digits, which only a code built from
  ///     unchecked characters can be.
  [[nodiscard]] auto verify(const GuestId& guest,
                            const VerificationCode& theirs)
      -> Expected<void, Refusal>;
  /// @brief Approves a guest for a seat and asks the console to seat them.
  ///
  /// May be called before verification has finished; the seat is then taken
  /// as soon as the guest is verified. The outcome arrives as events:
  /// PlayerChanged with PlayerSummary::reserved_seat, then PlayerSeated, or
  /// PlayerChanged at Progress::gone with Refusal::Kind::no_free_seat or
  /// Refusal::Kind::console_refused_seat.
  ///
  /// @param guest The guest to seat.
  /// @param seating How the console should seat them.
  /// @return Nothing once the approval is recorded.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  /// @retval Refusal::Kind::internal The guest is unknown.
  [[nodiscard]] auto give_seat(const GuestId& guest, GuestSeating seating = {})
      -> Expected<void, Refusal>;
  /// @brief Removes a guest from the shared session.
  ///
  /// Despite the name this ends the guest's whole visit, whether or not they
  /// hold a seat: a verified guest is told the host removed them, any seat
  /// they hold is freed and their connection closes. To let them back in,
  /// invite them again.
  ///
  /// @param guest The guest to remove.
  /// @return Nothing once the removal has begun.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  /// @retval Refusal::Kind::internal The guest is unknown.
  [[nodiscard]] auto take_back_seat(const GuestId& guest)
      -> Expected<void, Refusal>;
  /// @brief Remembers a guest's device under a nickname so it is recognised
  /// in later sessions.
  ///
  /// Unlike the other commands this one completes before it returns: it
  /// blocks while the DeviceStore saves the record, and while it removes the
  /// device that held the nickname when replace is Replace::yes. No event
  /// follows. It takes the session's lock, then the RememberedDevices lock.
  ///
  /// @param guest The guest whose device to remember. They must have been
  ///     verified in this session and not yet be gone.
  /// @param nickname The name to file the device under; see
  ///     RememberedDevices::normalised for what is accepted.
  /// @param replace Replace::yes to take the nickname from another device
  ///     that holds it, which forgets that device.
  /// @return The remembered device as saved.
  /// @retval RememberRefusal::Kind::unknown_device The guest is not one this
  ///     session is tracking.
  /// @retval RememberRefusal::Kind::not_verified The guest has not been
  ///     verified.
  /// @retval RememberRefusal::Kind::invalid_nickname The nickname is not
  ///     acceptable.
  /// @retval RememberRefusal::Kind::nickname_taken Another device holds the
  ///     nickname and replace is Replace::no; RememberRefusal::holder names
  ///     it.
  /// @retval RememberRefusal::Kind::store_unavailable The DeviceStore failed;
  ///     a device that held the nickname is still remembered.
  [[nodiscard]] auto remember(const GuestId& guest, std::string_view nickname,
                              Replace replace = Replace::no)
      -> Expected<RememberedDevice, RememberRefusal>;
  /// @brief Changes the number of seats while sharing.
  ///
  /// Guests whose seats no longer exist after the change are removed: one who
  /// was playing ends with EndReason::Kind::no_seat, one still being seated
  /// fails with Refusal::Kind::no_free_seat. SeatsChanged reports the new
  /// counts.
  ///
  /// @param rules The new capacity and local seat count. The allowed buttons
  ///     must be the same as when sharing began.
  /// @return Nothing once the new counts are in force.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  /// @retval Refusal::Kind::invalid_argument The rules change the allowed
  ///     buttons.
  [[nodiscard]] auto change(const SharingRules& rules)
      -> Expected<void, Refusal>;
  /// @brief Changes how the stream is forwarded to guests.
  ///
  /// The media settings apply to guests whose media starts after the call;
  /// the keyframe interval applies at once. The values in force are written
  /// to the log.
  ///
  /// @param tuning The new settings; see the warning on Tuning about its zero
  ///     defaults.
  /// @return Nothing on success.
  /// @retval Refusal::Kind::not_sharing Sharing has stopped.
  [[nodiscard]] auto tune(const Tuning& tuning) -> Expected<void, Refusal>;
  /// @brief The guests the session is tracking.
  ///
  /// Takes the session's lock briefly; safe from any thread, including from
  /// inside Observer::on_event.
  ///
  /// @return A snapshot, one entry per guest from the moment something is
  ///     known about them until they are gone. At most
  ///     Pacing::tracked_players entries.
  auto players() const -> std::vector<PlayerSummary>;
  /// @brief The seat counts.
  ///
  /// Takes the session's lock briefly; safe from any thread, including from
  /// inside Observer::on_event.
  ///
  /// @return A snapshot of the counts as they are now.
  auto seats() const -> SeatSummary;
  /// @brief Stops sharing.
  ///
  /// Guests are cut off and the play::Session carries on for the host alone.
  /// No further events are delivered, not even SharingEnded. Calling it again
  /// does nothing; afterwards every command answers
  /// Refusal::Kind::not_sharing.
  void stop_sharing();

 private:
  std::unique_ptr<Hosting> hosting_;
};

/// @brief Short name for SharedSession::Event.
using SharedSessionEvent = SharedSession::Event;
/// @brief Short name for SharedSession::Observer.
using SharedSessionObserver = SharedSession::Observer;

}  // namespace libchiaking::kakusan
