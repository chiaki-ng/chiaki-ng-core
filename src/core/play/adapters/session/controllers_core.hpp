/// @file
/// @brief Declares Controllers::Core, the per-seat queues of controller
/// states the session thread sends to the console.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/controllers.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/command_route.hpp"

#include <rigtorp/SPSCQueue.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief The implementation behind the public Controllers: one queue of
/// controller states per seat, flushed to the link by the session loop.
///
/// A frontend thread queues states; consecutive identical states are
/// collapsed, a state that only moves sticks or sensors replaces the last
/// queued one, and a full queue overwrites its last entry, so button and
/// touch edges are kept in order. A seat a Kakusan guest holds is
/// diverted: its states come from the guest through
/// Controllers::Core::forward and are sent at once. Every member locks one
/// mutex; sending happens outside it, and each sent state is settled in one
/// short critical section. Every seat's queue is allocated once at
/// construction, so queueing and sending a state allocate nothing. Owned by
/// the session core; the route must outlive it. Not copyable.
class Controllers::Core {
 public:
  /// @brief The most states queued per seat by default.
  static constexpr std::size_t queue_depth = 32;
  /// @brief How many times a state that failed to send is retried before it is
  /// dropped.
  static constexpr unsigned send_retries = 2;

  /// @brief Which queued states wake the session thread.
  enum class Waking : std::uint8_t {
    /// @brief Every state.
    every_sample,
    /// @brief Only a state whose feedback differs from the one that last woke
    /// it; the others go with the loop's next pass.
    changed_samples,
  };

  /// @brief Builds empty queues.
  ///
  /// @param threading Provider of the queues' mutex; must outlive this
  ///     object.
  /// @param route Where states go; must outlive this object.
  /// @param depth The most states queued per seat; at least 1.
  /// @param waking Which queued states wake the session thread.
  Core(platform::ThreadingProvider& threading, const CommandRoute& route,
       std::size_t depth = queue_depth, Waking waking = Waking::every_sample);
  /// @brief Not copyable; the queues are one per session.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; the queues are one per session.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Queues a seat's state and wakes the session thread.
  ///
  /// With Waking::changed_samples, a state whose feedback matches the one
  /// that last woke the thread is queued without a wake and goes with the
  /// loop's next pass.
  ///
  /// @param seat The seat.
  /// @param state The state.
  /// @return Nothing when queued or collapsed; InputRefusal::invalid for
  ///     buttons outside ControllerState::button_mask;
  ///     InputRefusal::not_running when commands are not accepted;
  ///     InputRefusal::not_owner when a guest holds the seat.
  [[nodiscard]] auto send(controllers::SeatNumber seat,
                          const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Sends a guest's state for a diverted seat at once, on the calling
  /// thread.
  ///
  /// A send failure is logged, not returned.
  ///
  /// @param seat The guest's seat.
  /// @param state The state.
  /// @return Nothing; InputRefusal::invalid for seat 0 or bad buttons;
  ///     InputRefusal::not_running when commands are not accepted;
  ///     InputRefusal::not_owner when the seat is not diverted.
  [[nodiscard]] auto forward(controllers::SeatNumber seat,
                             const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Drops a seat's queued states.
  ///
  /// @param seat The seat.
  void discard_pending(controllers::SeatNumber seat);
  /// @brief Hands a seat to a guest, dropping its queued states.
  ///
  /// @param seat The seat; seat 0 cannot be diverted.
  /// @return `true` when diverted; `false` for seat 0 or a seat already
  ///     diverted.
  auto divert(controllers::SeatNumber seat) -> bool;
  /// @brief Gives a seat back to the local player, dropping its queued states.
  ///
  /// @param seat The seat.
  void restore(controllers::SeatNumber seat);
  /// @brief Gives every seat back to the local player; queued states stay.
  void restore_all();
  /// @brief Reports whether a guest holds a seat.
  ///
  /// @param seat The seat.
  /// @return `true` while diverted.
  auto diverted(controllers::SeatNumber seat) const -> bool;
  /// @brief Counts the queued states of every seat.
  ///
  /// @return The total.
  auto pending() const -> std::size_t;
  /// @brief Counts a seat's queued states.
  ///
  /// @param seat The seat.
  /// @return The count.
  auto pending(controllers::SeatNumber seat) const -> std::size_t;
  /// @brief Sends queued states of every seat, up to the queue depth each;
  /// called by the session loop. Blocks on the link for each send.
  ///
  /// @return Nothing when every seat flushed; the first seat's failure
  ///     otherwise. A seat stops at its first failure, keeping the state to
  ///     retry.
  [[nodiscard]] auto flush()
      -> Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Converts a public state into the wire state of a seat.
  ///
  /// @param seat The seat, sent as the controller id.
  /// @param state The state.
  /// @return The wire state.
  static auto wire(controllers::SeatNumber seat,
                   const controllers::ControllerState& state) noexcept
      -> net::protocol::remote_play::ControllerState;
  /// @brief Converts a wire state into the public state.
  ///
  /// @param state The wire state.
  /// @return The state without its controller id.
  static auto typed(
      const net::protocol::remote_play::ControllerState& state) noexcept
      -> controllers::ControllerState;

 private:
  /// @brief One seat's queued states, oldest first, in storage allocated once.
  ///
  /// The newest state is held apart so it can still be replaced, and every
  /// older one waits in a fixed ring. Invariant: an older state is held only
  /// when a newest one is, and size() never exceeds the depth. Not
  /// synchronised: the owner's mutex guards it.
  class SeatQueue {
   public:
    /// @brief Makes an empty queue.
    ///
    /// @param depth The most states held; at least 1.
    explicit SeatQueue(std::size_t depth);

    /// @brief Queues a state: an identical one is collapsed, one with the
    /// same edges as the newest replaces it while an older state waits, and
    /// a full queue overwrites its newest state.
    ///
    /// @param state The state.
    void push(const controllers::ControllerState& state);
    /// @brief Reads the oldest state.
    ///
    /// @return A copy, or nothing when empty.
    auto front() -> std::optional<controllers::ControllerState>;
    /// @brief Drops the oldest state; nothing happens when empty.
    void pop() noexcept;
    /// @brief Drops every state and keeps the storage.
    void clear() noexcept;
    /// @brief Counts the states.
    ///
    /// @return The count.
    auto size() const noexcept -> std::size_t;
    /// @brief Whether nothing is queued.
    ///
    /// @return True when empty.
    auto empty() const noexcept -> bool;

   private:
    /// @brief The most states held.
    std::size_t depth_;
    /// @brief The older states, oldest first.
    rigtorp::SPSCQueue<controllers::ControllerState> older_;
    /// @brief The newest state, still open to replacement.
    std::optional<controllers::ControllerState> newest_;
  };

  /// @brief One seat's queue and send record.
  struct Queue {
    /// @brief States waiting to be sent, oldest first.
    SeatQueue samples;
    /// @brief Counts the clears, so a send that a clear overtook does not
    /// settle a newer state.
    std::uint64_t generation{};
    /// @brief Failed sends of the front state so far.
    unsigned retries{};
    /// @brief Whether the first accepted state was logged.
    bool reported{};
    /// @brief The last send error, so a repeated one is logged once.
    std::optional<net::protocol::remote_play::StreamError> refusal{};
    /// @brief Whether a guest holds the seat.
    bool diverted{};
    /// @brief The state that last woke the session thread; empty after the
    /// queue is cleared.
    std::optional<controllers::ControllerState> woken{};
  };

  /// @brief What a sent state calls for once its send has been settled.
  enum class Notice : std::uint8_t {
    /// @brief Nothing to log.
    none,
    /// @brief The seat's first accepted state.
    first_accepted,
    /// @brief A refusal different from the last one.
    newly_refused,
  };

  /// @brief What settling one send decided, acted on outside the lock.
  struct Settled {
    /// @brief What to log.
    Notice notice{Notice::none};
    /// @brief Whether the PS button went down with this state.
    bool system_button_pressed{};
    /// @brief The next state to send; empty when the seat has nothing more.
    std::optional<controllers::ControllerState> next;
    /// @brief The generation @p next was read in.
    std::uint64_t generation{};
  };

  /// @brief Reports whether two states have the same buttons and touch ids.
  ///
  /// @param a One state.
  /// @param b The other.
  /// @return `true` when no button or touch edge separates them.
  static auto same_edges(const controllers::ControllerState& a,
                         const controllers::ControllerState& b) noexcept
      -> bool;
  /// @brief Reports whether two states are identical in every field.
  ///
  /// @param a One state.
  /// @param b The other.
  /// @return `true` when identical.
  static auto same_sample(const controllers::ControllerState& a,
                          const controllers::ControllerState& b) noexcept
      -> bool;
  /// @brief Reports whether a state would send something the state that last
  /// woke the session thread did not.
  ///
  /// @param seat The seat.
  /// @param woken The state that last woke it, if any.
  /// @param state The new state.
  /// @return `true` without a previous wake or when buttons, triggers,
  ///     touches or sticks differ; motion alone does not wake, and rides
  ///     with the next pass.
  static auto worth_waking(
      controllers::SeatNumber seat,
      const std::optional<controllers::ControllerState>& woken,
      const controllers::ControllerState& state) noexcept -> bool;
  /// @brief Empties a queue and forgets its send record; the diversion stays.
  ///
  /// @param queue The queue.
  static void clear(Queue& queue);

  /// @brief Records the outcome of one send with the lock held: settles the
  /// queue's front when the send was of it, notes the log and PS button
  /// edge, and reads the next state.
  ///
  /// @param seat The seat.
  /// @param sample The state sent.
  /// @param result The link's result.
  /// @param generation The queue generation the state was read in; empty for
  ///     a guest's state, which was never queued.
  /// @return What to do outside the lock.
  auto settle(
      controllers::SeatNumber seat, const controllers::ControllerState& sample,
      const Expected<void, net::protocol::remote_play::StreamError>& result,
      std::optional<std::uint64_t> generation) -> Settled;
  /// @brief Logs a settled send and, on a LegacyCloudStream stream, opens or
  /// closes the pad's system menu when its PS button went down, since that
  /// server ignores the PS button in controller states and takes XMBCOMMAND
  /// for the pressing pad instead.
  ///
  /// Builds a log line only when there is one to write.
  ///
  /// @param seat The seat.
  /// @param sample The state sent.
  /// @param result The link's result.
  /// @param settled What settling the send decided.
  /// @param guest Whether it came from a guest.
  void follow_up(
      controllers::SeatNumber seat, const controllers::ControllerState& sample,
      const Expected<void, net::protocol::remote_play::StreamError>& result,
      const Settled& settled, bool guest);
  /// @brief Sends one seat's queued states, up to the queue depth.
  ///
  /// @param seat The seat.
  /// @return Nothing; the first failure, after which the state is kept unless
  ///     it used up its retries.
  auto flush(controllers::SeatNumber seat)
      -> Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Where states go.
  const CommandRoute& route_;
  /// @brief The most states queued per seat.
  std::size_t depth_;
  /// @brief Which queued states wake the session thread.
  Waking waking_;
  /// @brief Guards the queues.
  mutable platform::Mutex mutex_;
  /// @brief One queue per seat.
  std::array<Queue, controllers::SeatNumber::count> queues_;
  /// @brief Whether each seat's PS button was down in the last state sent.
  std::array<bool, controllers::SeatNumber::count> system_button_down_{};
};

}  // namespace libchiaking::play
