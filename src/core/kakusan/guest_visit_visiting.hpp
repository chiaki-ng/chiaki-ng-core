/// @file
/// @brief Declares GuestVisit::Visiting, the runtime state of one guest visit:
/// the protocol core, the frontend-facing phase and progress, the forwarded
/// stream as the frontend's video calls see it, and the steps waiting to be
/// delivered.
#pragma once

#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "src/core/kakusan/adapters/connections.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/pump_gate.hpp"
#include "src/core/kakusan/kakusan_core.hpp"
#include "src/core/kakusan/usecases/join.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace libchiaking::kakusan {

/// @brief Everything a GuestVisit owns, and the bookkeeping between the
/// protocol core, the run() loop and the frontend.
///
/// Holds the surroundings, the transport source, the GuestVisit::Core and the
/// usecases::Join that drives them; what the invitation offered and the host
/// key once proved; the pause run() sleeps on; the visit's phase and latest
/// progress; and the steps queued for the next delivery.
///
/// Three locks guard it. mutex() guards the state and is held only briefly,
/// never while the observer runs. gate() serialises pump turns and their
/// deliveries and is taken before mutex(). A separate stream lock guards the
/// forwarded stream and its last fault and statistics, so the frontend's
/// video calls never wait on the protocol. The cancelled flag is atomic.
///
/// Lifetime: made by usecases::Join::open and owned by the GuestVisit. The
/// destructor waits for any delivery in progress to finish. Neither copyable
/// nor movable.
class GuestVisit::Visiting {
 public:
  /// @brief One thing waiting to be delivered outside the lock.
  using Step = usecases::Join::Step;

  /// @brief What a progress change means for the frontend.
  struct Noted {
    /// @brief The visit reached playing for the first time: report
    /// Connected.
    bool connected{};
    /// @brief The visit reached gone: report Disconnected.
    bool finished{};
  };

  /// @brief What close() took out of the visiting, to release outside the
  /// lock.
  struct Closed {
    /// @brief Why the visit failed, if it did.
    std::optional<Refusal> refusal;
    /// @brief The protocol core, to destroy outside the lock.
    std::unique_ptr<Core> machine;
  };

  /// @brief Assembles the visiting and its Join.
  /// @param surroundings The Kakusan context; kept alive by the visiting.
  /// @param connections The transport source the core's link came from.
  /// @param machine The protocol core, with the invitation accepted.
  /// @param host What the invitation offered, with recognition filled in.
  /// @param ports The observer, media sink and optional pause; they must
  ///     outlive the visiting. A null pause is replaced by an internal one.
  /// @param pacing Timing of the run() loop.
  Visiting(std::shared_ptr<Surroundings> surroundings,
           std::unique_ptr<Connections> connections,
           std::unique_ptr<Core> machine, InvitationSummary host, Ports ports,
           Pacing pacing);
  /// @brief Waits for a delivery in progress on another thread to end, and
  /// drops the queued steps. Must not run inside a delivery.
  ~Visiting();
  /// @brief Not copyable.
  Visiting(const Visiting&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Visiting&) -> Visiting& = delete;

  /// @brief Tells whether a visit at a stage has been verified and is not
  /// gone.
  /// @param progress The stage.
  /// @return True from Progress::waiting_for_seat to Progress::leaving.
  static constexpr auto admitted(Progress progress) noexcept -> bool {
    return progress >= Progress::waiting_for_seat &&
           progress <= Progress::leaving;
  }
  /// @brief Wraps reply text as the public Reply.
  /// @param block The text.
  /// @return The reply.
  static auto reply(SecretString block) -> Reply;
  /// @brief Builds the public VisitOutcome.
  /// @param status How the visit ended.
  /// @param refusal Why it failed, for VisitOutcome::Status::visit_failed.
  /// @param media The media fault, for VisitOutcome::Status::media_failed.
  /// @return The outcome.
  static auto outcome(VisitOutcome::Status status,
                      std::optional<Refusal> refusal,
                      std::optional<media::MediaFault> media) -> VisitOutcome;

  /// @brief The use case that carries out commands and the run() loop.
  /// @return The Join; valid while the visiting lives.
  auto join() noexcept -> usecases::Join& { return join_; }
  /// @brief The state lock.
  /// @return The mutex; take gate() first when both are needed.
  auto mutex() const noexcept -> platform::Mutex& { return mutex_; }
  /// @brief The gate pump turns and deliveries pass through.
  /// @return The gate.
  auto gate() noexcept -> PumpGate& { return gate_; }
  /// @brief The protocol core. Use with mutex() held, and not after close()
  /// or abandon().
  /// @return The core.
  auto machine() const noexcept -> Core& { return *machine_; }
  /// @brief The Kakusan context.
  /// @return The surroundings.
  auto surroundings() const noexcept -> Surroundings& { return *surroundings_; }
  /// @brief What the invitation offered; never changes.
  /// @return The summary.
  auto host() const noexcept -> const InvitationSummary& { return host_; }
  /// @brief The pause run() sleeps on; safe to wake from any thread.
  /// @return The pause.
  auto pause() const noexcept -> Pause& { return pause_; }
  /// @brief The host device's key. Read with mutex() held.
  /// @return The key, once the host proved it.
  auto host_key() const noexcept -> const std::optional<IdentityKey>& {
    return host_key_;
  }
  /// @brief Records the host device's proved key.
  /// @param key The key.
  void host_identified(const IdentityKey& key) noexcept { host_key_ = key; }
  /// @brief Updates the last-seen time of the host's device if it is
  /// remembered; blocks while the store saves it.
  void host_seen() const;

  /// @brief Asks run() to leave at its next turn. Safe from any thread
  /// without a lock.
  void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
  /// @brief Tells whether cancel() was called. Safe without a lock.
  /// @return True once cancelled.
  auto cancelled() const noexcept -> bool {
    return cancelled_.load(std::memory_order_acquire);
  }

  /// @brief Tells whether commands are accepted. Read with mutex() held.
  /// @return True in Phase::open.
  auto open() const noexcept -> bool { return phase_ == Phase::open; }
  /// @brief Tells whether the departure has begun. Read with mutex() held.
  /// @return True in Phase::leaving.
  auto leaving() const noexcept -> bool { return phase_ == Phase::leaving; }
  /// @brief Tells whether the visit is verified and not gone. Read with
  /// mutex() held.
  /// @return True when so.
  auto host_admitted() const noexcept -> bool { return admitted(progress_); }
  /// @brief When run() gives up on a departure. Read with mutex() held.
  /// @return In monotonic microseconds; meaningful while leaving.
  auto leave_deadline_us() const noexcept -> std::uint64_t {
    return leave_deadline_us_;
  }
  /// @brief Marks run() as started.
  /// @return Whether it had been started before.
  auto already_ran() noexcept -> bool { return std::exchange(ran_, true); }
  /// @brief Enters Phase::leaving with a deadline.
  /// @param deadline_us When to give up, in monotonic microseconds.
  void leave_by(std::uint64_t deadline_us) noexcept;
  /// @brief Records the latest progress and, on reaching gone, the refusal
  /// and Phase::finished.
  /// @param changed The change.
  /// @return Whether Connected or Disconnected must follow.
  auto note(const VisitChanged& changed) -> Noted;
  /// @brief Closes a visit that never ran.
  /// @return The core, to destroy outside the lock.
  auto abandon() -> std::unique_ptr<Core>;
  /// @brief Closes the visit at the end of run(), keeping the stream's last
  /// fault and statistics.
  /// @return What to release outside the lock.
  auto close() -> Closed;

  /// @brief Queues a step for the next delivery.
  /// @param step The step.
  void queue(Step step) { queued_.push_back(std::move(step)); }
  /// @brief Takes every queued step.
  /// @return The steps, oldest first.
  auto take_queued() -> std::deque<Step>;
  /// @brief Tells whether steps are waiting.
  /// @return True when one is.
  auto has_queued() const noexcept -> bool { return !queued_.empty(); }

  /// @brief Picks up the core's current forwarded stream for the video calls,
  /// keeping the fault and statistics of the one it replaces. Call with
  /// mutex() held.
  void follow_stream();
  /// @brief The forwarded stream, for the frontend's video calls. Takes only
  /// the stream lock.
  /// @return The stream, or null.
  auto forwarded() const -> std::shared_ptr<play::ForwardedStream>;
  /// @brief The media fault of this visit. Takes only the stream lock.
  /// @return The first fault kept from an earlier stream, or the current
  ///     stream's fault, or nothing.
  auto fault() const -> std::optional<media::MediaFault>;
  /// @brief Video statistics. Takes only the stream lock.
  /// @return The current stream's, or the last ones kept.
  auto statistics() const -> media::VideoStatistics;

 private:
  /// @brief How far the visit has got from the frontend's point of view.
  enum class Phase : std::uint8_t {
    /// @brief Running; commands are accepted.
    open,
    /// @brief leave() or cancel() began the departure; run() keeps going
    /// until the visit is gone or Pacing::leave_deadline passes.
    leaving,
    /// @brief The visit reached Progress::gone; run() is wrapping up.
    finished,
    /// @brief run() has closed the visit, or it was abandoned before
    /// running.
    closed,
  };

  /// @brief The built-in Pause: a condition variable that wake() signals.
  /// Defined in the source file.
  class SignalPause;

  /// @brief The Kakusan context, kept alive.
  std::shared_ptr<Surroundings> surroundings_;
  /// @brief The transport source; outlives the core's link.
  std::unique_ptr<Connections> connections_;
  /// @brief What the invitation offered.
  InvitationSummary host_;
  /// @brief The host device's proved key.
  std::optional<IdentityKey> host_key_;
  /// @brief The built-in pause, when the frontend gave none.
  std::unique_ptr<Pause> own_pause_;
  /// @brief The pause in use.
  Pause& pause_;
  /// @brief Serialises pump turns and deliveries.
  PumpGate gate_;
  /// @brief Whether cancel() was called.
  std::atomic<bool> cancelled_{};
  /// @brief Guards the host key, the core, the queue, the phase and progress.
  mutable platform::Mutex mutex_;
  /// @brief The protocol core; null after close() or abandon().
  std::unique_ptr<Core> machine_;
  /// @brief Steps waiting for delivery.
  std::deque<Step> queued_;
  /// @brief The visit's phase.
  Phase phase_{Phase::open};
  /// @brief Whether run() was started.
  bool ran_{};
  /// @brief The latest progress reported.
  Progress progress_{Progress::waiting};
  /// @brief Why the visit failed, once gone.
  std::optional<Refusal> refusal_;
  /// @brief Whether Connected was reported.
  bool connected_{};
  /// @brief When a departure gives up, in monotonic microseconds.
  std::uint64_t leave_deadline_us_{};
  /// @brief Guards stream_, fault_ and statistics_.
  mutable platform::Mutex stream_mutex_;
  /// @brief The forwarded stream the video calls use.
  std::shared_ptr<play::ForwardedStream> stream_;
  /// @brief The first fault kept from a stream that has gone.
  std::optional<media::MediaFault> fault_;
  /// @brief Statistics kept from the last stream that has gone.
  media::VideoStatistics statistics_{};
  /// @brief The use case; declared last so it is built from the rest.
  usecases::Join join_;
};

}  // namespace libchiaking::kakusan
