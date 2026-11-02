/// @file
/// @brief Declares Controllers, the queue that sends each seat's controller
/// state into a session.
#pragma once

#include "libchiaking/detail/controllers/controller_state.hpp"
#include "libchiaking/detail/controllers/seat_number.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief Sends controller input for the seats of a session.
///
/// send queues a state and returns; the session's thread delivers queued states
/// while the stream is running, and keeps them queued while it is not. Each
/// seat has its own queue.
///
/// Obtained from Session::controllers and valid for as long as that Session.
/// All members may be called from any thread.
///
/// @see Seats
/// @see MediaSink::on_feedback
class Controllers {
 public:
  /// @brief Implementation behind the queue; owned by the session.
  class Core;

  /// @brief Number of states a session queues for each seat.
  static constexpr std::size_t default_depth = 64;

  /// @brief Binds the queue to the session's implementation.
  ///
  /// Frontends do not construct this; they use Session::controllers.
  ///
  /// @param core Implementation owned by the session. It must outlive this
  ///     object.
  explicit Controllers(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; a session has one input queue.
  Controllers(const Controllers&) = delete;
  /// @brief Not copy-assignable; a session has one input queue.
  auto operator=(const Controllers&) -> Controllers& = delete;

  /// @brief Queues the latest state of a seat's controller.
  ///
  /// Call it whenever the controller changes. States are delivered in order.
  /// To keep the queue short without losing a press or release, a state equal
  /// to the last queued one is dropped, and a state that differs from the
  /// last queued one only in analogue values replaces it when at least two
  /// states are waiting. When the queue is full the newest state replaces the
  /// last one.
  ///
  /// The call does not check that the seat is occupied.
  ///
  /// @param seat The seat the controller belongs to; seat 0 is the first
  ///     player.
  /// @param state The controller's complete current state.
  /// @return Nothing on success. The state is only queued; a failure to
  ///     deliver it is written to the session journal.
  /// @retval InputRefusal::invalid The state sets button bits outside
  ///     ControllerState::button_mask.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @retval InputRefusal::not_owner The seat is held by a shared guest,
  ///     whose input comes through SharedSeat::send.
  [[nodiscard]] auto send(controllers::SeatNumber seat,
                          const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Drops every state still queued for a seat.
  ///
  /// @param seat The seat whose queue is emptied.
  void discard_pending(controllers::SeatNumber seat);
  /// @brief Number of states waiting to be delivered, over all seats.
  ///
  /// @return Number of states waiting to be delivered, over all seats.
  auto pending() const -> std::size_t;
  /// @brief Number of states waiting to be delivered for one seat.
  ///
  /// @param seat The seat to count.
  /// @return Number of states waiting to be delivered for one seat.
  auto pending(controllers::SeatNumber seat) const -> std::size_t;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
