/// @file
/// @brief Declares FetchBoard, which keeps one account's fetch machine, its
/// latest state and the observers following the fetch in progress.
#pragma once

#include "libchiaking/detail/catalog/fetch_state.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace libchiaking::catalog {

/// @brief One account's fetch state and the observers that follow it.
///
/// The thread running a fetch feeds the board the attempt's events; the board
/// runs them through the fetch lifecycle and hands every new state to the
/// observers watching, in order, on that thread. A caller that joins a running
/// fetch watches it too and is told its current state at once. A cancelled
/// attempt is told only to the caller that led it: a joiner is not cancelled
/// with it and carries on into the attempt that follows.
///
/// A watch is told one attempt to its end and then nothing more, unless the
/// caller follows the next attempt with follow_again.
///
/// Each start-over moves the board to a new epoch, also while a fetch runs.
/// Every attempt records the epoch it began under, so a caller can tell an
/// answer from before its start-over and ask for a new attempt instead.
///
/// Safe to share between threads. One thread at a time takes a turn to tell the
/// queued states, calling each observer with no lock held, so states reach
/// every observer in the order the machine published them. On another thread,
/// watch, begin, follow_again, catch_up, start_over, the progress calls from
/// began to finished, and the end of a watch wait for that turn to end, so a
/// watch that has ended is never called afterwards; state and epoch do not
/// wait. The turn ends also when an observer throws; whatever it left queued
/// stays queued until a later call that publishes a state, starts a watch,
/// follows again or catches a watch up tells it. An observer may start over or
/// end a watch; that only queues what it publishes, and the turn tells it once
/// the observer returns. An observer that watches, begins, follows again or
/// catches up, as starting or joining a fetch would, ends the process through
/// std::terminate. The latest state is the one last told, kept under a separate
/// lock, and can be read from an observer.
class FetchBoard {
 public:
  /// @brief One caller following the board until it goes out of scope.
  class Watch {
   public:
    /// @brief Not copyable: it stands for one registration.
    Watch(const Watch&) = delete;
    /// @brief Not copyable: it stands for one registration.
    auto operator=(const Watch&) -> Watch& = delete;

   private:
    friend class FetchBoard;

    /// @brief Owns one registration id on a board from the moment it is
    /// issued.
    class Registration {
     public:
      /// @brief Takes the next id of a board.
      ///
      /// @param board The board followed.
      explicit Registration(FetchBoard& board);
      /// @brief Stops following, after any turn in progress on another
      /// thread.
      ~Registration();
      /// @brief Not copyable: it stands for one registration.
      Registration(const Registration&) = delete;
      /// @brief Not copyable: it stands for one registration.
      auto operator=(const Registration&) -> Registration& = delete;

      /// @brief The registration's id.
      ///
      /// @return The id taken at construction.
      auto id() const noexcept -> std::uint64_t { return id_; }

     private:
      /// @brief The board followed.
      FetchBoard& board_;
      /// @brief The registration's id.
      std::uint64_t id_;
    };

    /// @brief Registers a caller and tells it the running fetch's state.
    ///
    /// @param board The board followed.
    /// @param observer The caller's observer.
    Watch(FetchBoard& board, const FetchObserver& observer);

    /// @brief The registration, released when the watch ends or fails to
    /// finish registering.
    Registration registration_;
  };

  /// @brief An attempt that has begun.
  struct Begun {
    /// @brief The attempt.
    std::uint64_t attempt{};
    /// @brief The start-over epoch it began under.
    std::uint64_t epoch{};
    /// @brief Whether a start-over came while the previous attempt ran, so
    /// what that attempt kept must be discarded before this one fetches.
    bool starts_over{};
  };

  /// @brief Makes an idle board.
  ///
  /// @param threading Makes the board's locks and its condition variable;
  ///     must outlive it.
  /// @param clock The monotonic clock the condition variable measures on;
  ///     must outlive the board.
  FetchBoard(platform::ThreadingProvider& threading,
             const platform::ClockProvider& clock);
  /// @brief Not copyable: it owns locks and a machine.
  FetchBoard(const FetchBoard&) = delete;
  /// @brief Not copyable: it owns locks and a machine.
  auto operator=(const FetchBoard&) -> FetchBoard& = delete;
  /// @brief Releases the machine.
  ~FetchBoard();

  /// @brief The state last told to the observers.
  ///
  /// @return A copy of it.
  auto state() const -> FetchState;
  /// @brief Starts following the board.
  ///
  /// When a fetch is running, the observer is told its current state before
  /// this returns. Called from an observer, it ends the process through
  /// std::terminate.
  ///
  /// @param observer Told every later state while the watch lives; may be
  ///     empty; must outlive the watch.
  /// @return The watch.
  auto watch(const FetchObserver& observer) -> Watch;
  /// @brief Starts a new attempt led by a watching caller.
  ///
  /// @param leader The leading caller's watch.
  /// @return The attempt, or nothing when one is already running.
  auto begin(const Watch& leader) -> std::optional<Begun>;
  /// @brief Follows the next attempt with a watch whose attempt has ended.
  ///
  /// When a fetch is running, the watch's observer is told its current state
  /// before this returns.
  ///
  /// @param watch The watch.
  void follow_again(const Watch& watch);
  /// @brief Tells a watch the end of the attempt whose answer its caller
  /// takes, when the watch missed it because it entered just as an earlier
  /// attempt ended.
  ///
  /// Does nothing when the watch was told that attempt, or when a newer
  /// attempt has begun since.
  ///
  /// @param watch The watch.
  /// @param attempt The attempt whose answer the caller takes.
  void catch_up(const Watch& watch, std::uint64_t attempt);
  /// @brief The current start-over epoch.
  ///
  /// @return The number of start-overs so far.
  auto epoch() const -> std::uint64_t;
  /// @brief The attempt's next step begins.
  ///
  /// @param attempt The attempt.
  /// @param step The step.
  /// @param activity What it starts doing, when known.
  void began(std::uint64_t attempt, FetchStep step,
             std::optional<FetchActivity> activity);
  /// @brief The attempt's current step does something else.
  ///
  /// @param attempt The attempt.
  /// @param activity The new activity.
  void doing(std::uint64_t attempt, FetchActivity activity);
  /// @brief The attempt's current activity has read more pages.
  ///
  /// @param attempt The attempt.
  /// @param read Pages read so far.
  /// @param expected Pages expected, when known.
  void pages(std::uint64_t attempt, std::uint32_t read,
             std::optional<std::uint32_t> expected);
  /// @brief The attempt's trusted-time step asks another time server.
  ///
  /// @param attempt The attempt.
  /// @param host The server's host name.
  void asking(std::uint64_t attempt, std::string_view host);
  /// @brief The attempt's current step has ended.
  ///
  /// @param attempt The attempt.
  /// @param outcome How it was answered.
  void ended(std::uint64_t attempt, StepOutcome outcome);
  /// @brief The attempt ends without games, or is cancelled.
  ///
  /// @param attempt The attempt.
  /// @param why Why; a cancellation ends it as cancelled.
  void failed(std::uint64_t attempt, const Unanswered& why);
  /// @brief The attempt ends with games.
  ///
  /// @param attempt The attempt.
  void finished(std::uint64_t attempt);
  /// @brief Starts a new epoch, as when the account starts over.
  ///
  /// An ended fetch returns to idle. A running fetch is left to end; the
  /// attempt after it is told to discard what it kept.
  void start_over();

 private:
  /// @brief The fetch lifecycle and its context.
  struct Machine;
  /// @brief One caller following the board.
  struct Watcher {
    /// @brief The registration's id.
    std::uint64_t id{};
    /// @brief Told every state; never null, possibly empty.
    const FetchObserver* observer{};
    /// @brief Whether it is still told states; cleared once it has been told
    /// the end of an attempt.
    bool following{true};
    /// @brief The attempt of the last state it was told; 0 before any.
    std::uint64_t told{};
  };
  /// @brief One state waiting to be told.
  struct Telling {
    /// @brief The registration's id; 0 for a state told to no watcher.
    std::uint64_t watch{};
    /// @brief The watcher's observer; null for a state told to no watcher,
    /// never empty.
    const FetchObserver* observer{};
    /// @brief The state.
    FetchState state;
  };

  /// @brief Whether a call may run from inside an observer.
  enum class Reentry : std::uint8_t {
    /// @brief It may, and only queues what it publishes.
    allowed,
    /// @brief It may not; the process ends through std::terminate.
    refused,
  };

  /// @brief Marks the calling thread as the one telling the queued states,
  /// for its scope.
  ///
  /// The outermost turn on a thread first waits for any other thread's turn
  /// to end; a turn nested inside an observer on the same thread neither
  /// waits nor ends the turn.
  class Turn {
   public:
    /// @brief Starts a turn.
    ///
    /// @param board The board.
    /// @param lock The held delivery lock; must outlive the turn.
    /// @param reentry Whether the turn may nest inside an observer; a refused
    ///     nesting ends the process through std::terminate.
    Turn(FetchBoard& board, platform::Lock& lock, Reentry reentry);
    /// @brief Ends the turn when it was the outermost one, taking the
    /// delivery lock back first when an observer threw without it.
    ~Turn();
    /// @brief Not copyable; a turn is scoped.
    Turn(const Turn&) = delete;
    /// @brief Not copy-assignable; a turn is scoped.
    auto operator=(const Turn&) -> Turn& = delete;

    /// @brief Calls the observers of the queued states in order, releasing
    /// the delivery lock around each call and publishing each state as it is
    /// told; does nothing in a nested turn, whose outermost turn tells what
    /// was queued once the observer returns.
    void tell_queued();

   private:
    /// @brief The board.
    FetchBoard& board_;
    /// @brief The held delivery lock.
    platform::Lock& lock_;
    /// @brief Whether this turn marked the thread.
    bool outermost_;
  };

  /// @brief Queues a state for one watcher and records it as told; called
  /// with the delivery lock held.
  ///
  /// @param watcher The watcher.
  /// @param state The state.
  void tell(Watcher& watcher, const FetchState& state);
  /// @brief Whether a state ends its attempt.
  ///
  /// @param state The state.
  /// @return True for FetchStep::ready, FetchStep::unanswered and
  ///     FetchStep::cancelled.
  static auto ends_its_attempt(const FetchState& state) noexcept -> bool;

  /// @brief Feeds an event to the machine and delivers what it published.
  ///
  /// @tparam Event The event type.
  /// @param event The event.
  template <class Event>
  void feed(const Event& event);
  /// @brief Queues every state the machine published for the watchers, the
  /// cancelled state for the leader alone; called with the delivery lock
  /// held.
  void deliver();
  /// @brief Issues the next registration id.
  ///
  /// @return The id.
  auto next_id() -> std::uint64_t;
  /// @brief Registers an observer under an issued id and tells it the running
  /// fetch's state.
  ///
  /// @param id The id.
  /// @param observer The observer.
  void enrol(std::uint64_t id, const FetchObserver& observer);
  /// @brief Ends a registration and drops what is queued for it, after any
  /// turn in progress on another thread.
  ///
  /// @param id The registration's id.
  void unwatch(std::uint64_t id);

  /// @brief Held while the machine runs and states are queued, and released
  /// while an observer is told.
  mutable platform::Mutex delivery_;
  /// @brief Signalled when the outermost turn ends.
  platform::ConditionVariable turn_ended_;
  /// @brief Guards the latest state.
  mutable platform::Mutex latest_;
  /// @brief The machine; guarded by the delivery lock.
  std::unique_ptr<Machine> machine_;
  /// @brief The state last told; guarded by its own lock.
  FetchState state_{};
  /// @brief The callers following; guarded by the delivery lock.
  std::vector<Watcher> watchers_{};
  /// @brief The next registration's id; guarded by the delivery lock.
  std::uint64_t next_watch_{1};
  /// @brief The registration that leads the running attempt.
  std::optional<std::uint64_t> leader_{};
  /// @brief The start-over epoch; guarded by the delivery lock.
  std::uint64_t epoch_{};
  /// @brief Whether a start-over came while an attempt ran; guarded by the
  /// delivery lock.
  bool start_over_pending_{};
  /// @brief The states waiting to be told; guarded by the delivery lock.
  std::deque<Telling> queued_{};
  /// @brief The thread whose turn it is; names none between turns; guarded
  /// by the delivery lock.
  platform::ThreadIdentity teller_{};
};

}  // namespace libchiaking::catalog
