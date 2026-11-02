/// @file
/// @brief Declares SharedSession::Hosting, the runtime state of one shared
/// session: the protocol core, the guests as the frontend sees them, the
/// console seats they hold, and the steps waiting to be delivered.
#pragma once

#include "libchiaking/detail/kakusan/shared_session.hpp"
#include "src/core/kakusan/adapters/connections.hpp"
#include "src/core/kakusan/adapters/pump_gate.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"
#include "src/core/kakusan/kakusan_core.hpp"
#include "src/core/kakusan/usecases/host.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace libchiaking::kakusan {

/// @brief Everything a SharedSession owns, and the bookkeeping between the
/// protocol core, the play::Session being shared and the frontend.
///
/// Holds the surroundings, the transport source, the SharedSession::Core and
/// the usecases::Host that drives them, plus a summary of each tracked guest
/// (at most Pacing::tracked_players), the console seats held for guests, and
/// the steps queued for the next delivery.
///
/// Two locks guard it. mutex() guards all state except the atomics and is
/// held only briefly, never while the observer runs. gate() serialises pump
/// turns and their deliveries and is always taken before mutex(). The
/// atomics (the tapped core and the datagram flags) are read and written
/// without either lock by the stream receive path.
///
/// Lifetime: made by usecases::Host::open and owned by the SharedSession.
/// The destructor stops sharing, then waits for any delivery in progress to
/// finish before the members go. Neither copyable nor movable.
class SharedSession::Hosting {
 public:
  /// @brief One thing waiting to be delivered outside the lock.
  using Step = usecases::Host::Step;

  /// @brief One guest as the frontend sees them, and how to seat them.
  struct Tracked {
    /// @brief The guest's latest summary.
    PlayerSummary player;
    /// @brief How the host asked for them to be seated.
    GuestSeating seating{};
  };

  /// @brief A console seat held for a guest: being seated, or seated.
  ///
  /// Destroying it gives the console seat up.
  struct Seated {
    /// @brief The guest.
    GuestId guest;
    /// @brief Their Kakusan seat.
    SeatGrant grant;
    /// @brief The console seat, which carries their input.
    play::SharedSeat console;
    /// @brief Whether the console has seated them.
    bool joined{};
  };

  /// @brief What close() took out of the hosting, to be released outside the
  /// lock.
  struct Closed {
    /// @brief The share, to stop.
    std::optional<play::Sharing> sharing;
    /// @brief The console seats held, to give up.
    std::vector<Seated> seated;
    /// @brief Whether the stream was being shared, so the core must be told
    /// it ended.
    bool streaming{};
  };

  /// @brief Assembles the hosting and its Host.
  /// @param surroundings The Kakusan context; kept alive by the hosting.
  /// @param connections The transport source the core's links come from.
  /// @param machine The protocol core.
  /// @param observer Receives the events; must outlive the hosting.
  /// @param pacing Timing and limits.
  Hosting(std::shared_ptr<Surroundings> surroundings,
          std::unique_ptr<Connections> connections,
          std::unique_ptr<Core> machine, Observer& observer, Pacing pacing);
  /// @brief Stops sharing, waits for a delivery in progress on another thread
  /// to end, and drops the queued steps. Must not run inside a delivery.
  ~Hosting();
  /// @brief Not copyable.
  Hosting(const Hosting&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Hosting&) -> Hosting& = delete;

  /// @brief Tells whether a guest at a stage has been verified and is not
  /// gone.
  /// @param progress The stage.
  /// @return True from Progress::waiting_for_seat to Progress::leaving.
  static constexpr auto admitted(Progress progress) noexcept -> bool {
    return progress >= Progress::waiting_for_seat &&
           progress <= Progress::leaving;
  }
  /// @brief Tells whether a guest at a stage may be receiving the stream.
  /// @param progress The stage.
  /// @return True at waiting_for_seat, getting_ready and playing.
  static constexpr auto forwarding(Progress progress) noexcept -> bool {
    return progress == Progress::waiting_for_seat ||
           progress == Progress::getting_ready || progress == Progress::playing;
  }

  /// @brief The use case that carries out commands and console callbacks.
  /// @return The Host; valid while the hosting lives.
  auto host() noexcept -> usecases::Host& { return host_; }
  /// @brief The state lock.
  /// @return The mutex; take gate() first when both are needed.
  auto mutex() const noexcept -> platform::Mutex& { return mutex_; }
  /// @brief The gate pump turns and deliveries pass through.
  /// @return The gate.
  auto gate() noexcept -> PumpGate& { return gate_; }
  /// @brief The protocol core. Use with mutex() held.
  /// @return The core.
  auto machine() const noexcept -> Core& { return *machine_; }
  /// @brief The Kakusan context.
  /// @return The surroundings.
  auto surroundings() const noexcept -> Surroundings& { return *surroundings_; }

  /// @brief Tells whether commands are still accepted. Read with mutex()
  /// held.
  /// @return False once sharing stopped or the session ended.
  auto live() const noexcept -> bool { return live_; }
  /// @brief Tells whether the stream started being shared. Read with mutex()
  /// held.
  /// @return True after StreamShared.
  auto streaming() const noexcept -> bool { return streaming_; }
  /// @brief Tells whether stop_sharing() ran, after which nothing more is
  /// delivered. Read with mutex() held.
  /// @return True once stopped.
  auto stopped() const noexcept -> bool { return stopped_; }
  /// @brief The session being shared. Read with mutex() held.
  /// @return The session, or null once sharing stopped or the session ended.
  auto session() const noexcept -> play::SessionShare* { return session_; }
  /// @brief Records that the session accepted the share.
  /// @param sharing Keeps the share alive.
  /// @param session The session; must outlive the hosting or end first.
  void began(play::Sharing sharing, play::SessionShare& session);
  /// @brief Records that the session refused the share; nothing is ever
  /// delivered.
  void never_began() noexcept;
  /// @brief Records that the stream started being shared.
  void stream_shared() noexcept { streaming_ = true; }
  /// @brief Stops accepting commands because the session ended.
  /// @return Whether commands were accepted until now.
  auto end() noexcept -> bool { return std::exchange(live_, false); }
  /// @brief Forgets the session as it ends, and hands over the console seats
  /// to be given up outside the lock.
  /// @return The console seats held.
  auto leave_session() -> std::vector<Seated>;
  /// @brief Stops sharing: commands stop being accepted, nothing more is
  /// delivered and the tracked guests are forgotten.
  /// @return What to release outside the lock, or nothing when already
  ///     stopped.
  auto close() -> std::optional<Closed>;
  /// @brief Draws the tag that names the next console seat request.
  /// @return A tag unique within this share, counting from one.
  auto next_guest_tag() noexcept -> play::GuestTag {
    return play::GuestTag{++guest_tags_};
  }

  /// @brief Sets the core the stream receive path forwards datagrams to.
  /// @param machine The core, or null to stop forwarding.
  void tap(Core* machine) noexcept {
    tap_.store(machine, std::memory_order_release);
  }
  /// @brief The core datagrams go to. Safe without a lock.
  /// @return The core, or null.
  auto tapped() const noexcept -> Core* {
    return tap_.load(std::memory_order_acquire);
  }
  /// @brief Notes whether any guest may be receiving the stream, so that a
  /// forwarded datagram wakes the pump.
  /// @param wanted True when one may.
  void forward_datagrams(bool wanted) noexcept {
    wanted_.store(wanted, std::memory_order_release);
  }
  /// @brief Notes that a datagram was forwarded and the pump has work. Safe
  /// without a lock.
  void datagram_forwarded() noexcept {
    if (wanted_.load(std::memory_order_acquire))
      pending_.store(true, std::memory_order_release);
  }
  /// @brief Clears the note, at the start of a pump turn.
  void datagrams_pumped() noexcept {
    pending_.store(false, std::memory_order_release);
  }
  /// @brief Tells whether a forwarded datagram is waiting for the pump.
  /// @return True when one is.
  auto datagrams_pending() const noexcept -> bool {
    return pending_.load(std::memory_order_acquire);
  }

  /// @brief Queues a step for the next delivery.
  /// @param step The step.
  void queue(Step step) { queued_.push_back(std::move(step)); }
  /// @brief Takes every queued step.
  /// @return The steps, oldest first.
  auto take_queued() -> std::deque<Step>;
  /// @brief Tells whether steps are waiting.
  /// @return True when one is.
  auto has_queued() const noexcept -> bool { return !queued_.empty(); }

  /// @brief Finds a tracked guest.
  /// @param guest The guest.
  /// @return The row, or null.
  auto row(const GuestId& guest) -> Tracked*;
  /// @brief Records how a guest should be seated, tracking them if there is
  /// room.
  /// @param guest The guest.
  /// @param seating How to seat them.
  void seat_as(const GuestId& guest, const GuestSeating& seating);
  /// @brief Records a guest's device, tracking them if there is room.
  /// @param guest The guest.
  /// @param device Their identity key.
  /// @param fingerprint Its fingerprint.
  void identified(const GuestId& guest, const IdentityKey& device,
                  const Fingerprint& fingerprint);
  /// @brief Records a guest's new summary, keeping their device; tracks a new
  /// guest if there is room, or notes the overflow; forgets a gone guest.
  /// @param player The new summary, without device or seat fields.
  /// @return The summary as shown, with device and seat fields filled in.
  auto changed(PlayerSummary player) -> PlayerSummary;
  /// @brief Fills in the console seat a guest holds or is being given.
  /// @param player The summary.
  /// @return It with PlayerSummary::console_seat, or reserved_seat and grant
  ///     while being seated.
  auto shown(PlayerSummary player) const -> PlayerSummary;
  /// @brief Every tracked guest as shown.
  /// @return The summaries.
  auto players() const -> std::vector<PlayerSummary>;
  /// @brief Tells whether any guest is verified and not gone, or the tracking
  /// overflowed, so the loop should not sleep long.
  /// @return True when so.
  auto guests_admitted() const noexcept -> bool;
  /// @brief Tells whether any guest may be receiving the stream, or the
  /// tracking overflowed.
  /// @return True when so.
  auto guests_forwarding() const noexcept -> bool;
  /// @brief A verified guest's device, for remembering it.
  /// @param guest The guest.
  /// @return The identity key.
  /// @retval RememberRefusal::Kind::unknown_device The guest is not tracked.
  /// @retval RememberRefusal::Kind::not_verified The guest has no device yet
  ///     or is not admitted.
  [[nodiscard]] auto verified_device(const GuestId& guest)
      -> Expected<IdentityKey, RememberRefusal>;

  /// @brief Turns the core's invitation into the public Invitation.
  /// @param ready The core's output; its text is taken.
  /// @return The invitation, with no recognition filled in.
  auto invitation(Core::InvitationReady& ready) const -> Invitation;
  /// @brief Updates the last-seen time of a remembered device; blocks while
  /// the store saves it.
  /// @param device The device.
  void seen(const IdentityKey& device) const;

  /// @brief Records a console seat requested for a guest.
  /// @param guest The guest.
  /// @param grant Their Kakusan seat.
  /// @param console The console seat, not yet joined.
  void reserve(const GuestId& guest, const SeatGrant& grant,
               play::SharedSeat console);
  /// @brief Finds the guest the console is still seating in a seat.
  /// @param seat The console seat.
  /// @return The entry, or null.
  auto awaiting(controllers::SeatNumber seat) -> Seated*;
  /// @brief Finds the console seat held for a guest.
  /// @param guest The guest.
  /// @return The entry, or null.
  auto seat_of(const GuestId& guest) -> Seated*;
  /// @brief Removes the guest still being seated in a console seat.
  /// @param seat The console seat.
  /// @return The entry, to destroy outside the lock, or nothing.
  auto take_awaiting(controllers::SeatNumber seat) -> std::optional<Seated>;
  /// @brief Removes whoever holds a console seat.
  /// @param seat The console seat.
  /// @return The entry, to destroy outside the lock, or nothing.
  auto take(controllers::SeatNumber seat) -> std::optional<Seated>;
  /// @brief Removes the console seat held for a guest.
  /// @param guest The guest.
  /// @return The entry, to destroy outside the lock, or nothing.
  auto take(const GuestId& guest) -> std::optional<Seated>;

 private:
  /// @brief Removes one entry from the seats held.
  /// @param found The entry, or the end iterator.
  /// @return The entry, or nothing for the end iterator.
  auto taken(std::vector<Seated>::iterator found) -> std::optional<Seated>;

  /// @brief The Kakusan context, kept alive.
  std::shared_ptr<Surroundings> surroundings_;
  /// @brief The transport source; outlives the core's links.
  std::unique_ptr<Connections> connections_;
  /// @brief Timing and limits.
  Pacing pacing_;
  /// @brief Serialises pump turns and deliveries.
  PumpGate gate_;
  /// @brief Guards every member below that is not atomic.
  mutable platform::Mutex mutex_;
  /// @brief The protocol core; never null.
  std::unique_ptr<Core> machine_;
  /// @brief The core datagrams go to; null before sharing begins and after it
  /// stops.
  std::atomic<Core*> tap_{};
  /// @brief Whether any guest may be receiving the stream.
  std::atomic<bool> wanted_{};
  /// @brief Whether a datagram was forwarded since the last pump turn.
  std::atomic<bool> pending_{};
  /// @brief Steps waiting for delivery.
  std::deque<Step> queued_;
  /// @brief The session being shared, or null.
  play::SessionShare* session_{};
  /// @brief Keeps the share alive.
  std::optional<play::Sharing> sharing_;
  /// @brief The tracked guests, at most Pacing::tracked_players.
  std::vector<Tracked> tracked_;
  /// @brief The console seats held for guests.
  std::vector<Seated> seated_;
  /// @brief The last guest tag drawn.
  std::uint64_t guest_tags_{};
  /// @brief Whether a guest could not be tracked for want of room.
  bool tracked_overflow_{};
  /// @brief Whether commands are accepted.
  bool live_{true};
  /// @brief Whether the stream is being shared.
  bool streaming_{};
  /// @brief Whether sharing was stopped.
  bool stopped_{};
  /// @brief The use case; declared last so it is built from the rest.
  usecases::Host host_;
};

}  // namespace libchiaking::kakusan
