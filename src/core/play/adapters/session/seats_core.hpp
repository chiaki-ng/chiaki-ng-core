/// @file
/// @brief Declares Seats::Core, the session's view of who sits in each of
/// the four controller seats.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/play/seats.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "src/core/play/adapters/session/controllers_core.hpp"
#include "src/core/play/adapters/session/seat_requests.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play {

/// @brief The implementation behind the public Seats: the four controller
/// seats of a session, who holds each, and the join and leave requests
/// queued for the console.
///
/// Seat 0 is the player's own controller and is always held. Extra seats are
/// asked for by the local player or given to a Kakusan guest; the request
/// is queued and the session loop sends it while streaming. The seat states
/// follow what the link reports: a passcode demand, a join, a drop. A guest's
/// seat diverts that seat's controller input to the guest. Two mutexes guard
/// the seats and the request queue, taken in that order. Owned by the session
/// core; the route and controllers must outlive it. Not copyable.
class Seats::Core {
 public:
  /// @brief How long the console has to answer a join, in microseconds.
  static constexpr std::uint64_t join_timeout_us = 10'000'000;
  /// @brief How many passcodes a PS5 seat may try.
  static constexpr std::uint8_t passcode_attempt_limit = 3;
  /// @brief The wire controller type of a DualShock 4.
  static constexpr std::uint8_t dualshock4_wire = 2;
  /// @brief The wire controller type of a DualSense.
  static constexpr std::uint8_t dualsense_wire = 6;
  /// @brief The most seat requests queued by default.
  static constexpr std::size_t request_depth = 8;

  /// @brief Which couch protocol the other end speaks.
  enum class ConsoleKind : std::uint8_t {
    /// @brief A PS4 over Remote Play: pads join by pad number and controller
    /// type.
    remote_play_ps4,
    /// @brief A PS5 over Remote Play: pads join as a PSN account and may need a
    /// passcode.
    remote_play_ps5,
    /// @brief A cloud stream: pads open without asking the server.
    cloud,
  };

  /// @brief Where a seat stands on the console's side of the couch protocol.
  enum class ConsoleSeatState : std::uint8_t {
    /// @brief Nobody is in the seat.
    empty,
    /// @brief A join was sent and the console has not answered.
    join_requested,
    /// @brief The console asked for the seat's passcode.
    entering_passcode,
    /// @brief The pad is admitted and streams input.
    joined,
  };

  /// @brief The console-side couch protocol, templated on the active session it
  /// drives; defined in seats_console.hpp.
  template <class Active>
  class Console;

  /// @brief Builds the seats with seat 0 joining for the first controller.
  ///
  /// @param threading Provider of the seats' mutexes; must outlive this
  ///     object.
  /// @param route Where requests go; must outlive this object.
  /// @param controllers The controller queues; must outlive this object.
  /// @param first_controller The kind of the player's own controller.
  /// @param depth The most requests queued; at least 1.
  Core(platform::ThreadingProvider& threading, const CommandRoute& route,
       Controllers::Core& controllers,
       controllers::ControllerKind first_controller,
       std::size_t depth = request_depth);
  /// @brief Not copyable; the seats are one per session.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; the seats are one per session.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Asks for an extra seat for the local player.
  ///
  /// @param seat The seat; not seat 0.
  /// @param controller The kind of controller.
  /// @param as The PSN account the pad joins as; needed on a PS5.
  /// @return Nothing when queued; InputRefusal::invalid for seat 0;
  ///     InputRefusal::busy when the seat is held or the queue is full;
  ///     InputRefusal::not_running when commands are not accepted.
  [[nodiscard]] auto add_player(controllers::SeatNumber seat,
                                controllers::ControllerKind controller,
                                std::optional<AccountId> as)
      -> Expected<void, InputRefusal>;
  /// @brief Gives up a seat the local player holds.
  ///
  /// @param seat The seat; not seat 0.
  /// @return Nothing when queued; InputRefusal::invalid for seat 0;
  ///     InputRefusal::not_owner for a guest's seat; InputRefusal::busy when
  ///     the queue is full; InputRefusal::not_running when commands are not
  ///     accepted.
  [[nodiscard]] auto remove_player(controllers::SeatNumber seat)
      -> Expected<void, InputRefusal>;
  /// @brief Queues the passcode the console asked for and wakes the session
  /// thread.
  ///
  /// @param seat The seat.
  /// @param code The code.
  /// @return Nothing when queued, replacing an earlier code not yet sent;
  ///     InputRefusal::invalid for a malformed code; InputRefusal::not_running
  ///     when commands are not accepted.
  [[nodiscard]] auto enter_passcode(controllers::SeatNumber seat,
                                    AccessCode code)
      -> Expected<void, InputRefusal>;
  /// @brief Reads one seat.
  ///
  /// @param seat The seat.
  /// @return Its state, holder and controller; an empty seat when nobody holds
  ///     it.
  auto seat(controllers::SeatNumber seat) const -> Seat;
  /// @brief Reads every seat.
  ///
  /// @return The four seats in order.
  auto all() const -> std::array<Seat, controllers::SeatNumber::count>;

  /// @brief Gives a seat to a Kakusan guest and diverts its controller
  /// input to them.
  ///
  /// @param seat The seat.
  /// @param guest The guest.
  /// @param controller The kind of controller.
  /// @param as The PSN account the pad joins as.
  /// @return Nothing when queued; SeatRefusal::no_free_seat when the seat is
  ///     diverted, held, seat 0 or full; a not-sent refusal when commands are
  ///     not accepted.
  [[nodiscard]] auto seat_guest(controllers::SeatNumber seat, GuestTag guest,
                                controllers::ControllerKind controller,
                                std::optional<AccountId> as)
      -> Expected<void, SeatRefusal>;
  /// @brief Takes a seat back from a guest, queues its removal and restores
  /// its input; ignored for an empty or local seat.
  ///
  /// @param seat The seat.
  void release_guest(controllers::SeatNumber seat);
  /// @brief Finds an extra seat nobody holds.
  ///
  /// @return The lowest empty seat from 1; empty when all are held.
  auto free_seat() const -> std::optional<controllers::SeatNumber>;

  /// @brief Follows the session: seat 0 plays once the session plays, and
  /// every seat empties when it ends.
  ///
  /// @param state The session's state.
  void session_is(SessionState state);
  /// @brief Empties every extra seat after the link was lost; seat 0 goes back
  /// to joining.
  ///
  /// @return The seats that were emptied.
  auto link_lost() -> std::vector<controllers::SeatNumber>;
  /// @brief Records that the console asked for a seat's passcode.
  ///
  /// @param seat The seat.
  void passcode_needed(controllers::SeatNumber seat);
  /// @brief Records that a seat's pad was admitted.
  ///
  /// @param seat The seat.
  void joined(controllers::SeatNumber seat);
  /// @brief Empties a seat, restoring its input when a guest held it.
  ///
  /// @param seat The seat; seat 0 is ignored.
  void emptied(controllers::SeatNumber seat);
  /// @brief Empties a seat the console dropped, unless it is still joining.
  ///
  /// @param seat The seat.
  /// @return `false` when the seat is joining and stays; `true` after emptying.
  auto dropped(controllers::SeatNumber seat) -> bool;

  /// @brief Reports whether a passcode waits to be sent.
  ///
  /// @return `true` while one is queued.
  auto passcode_pending() const -> bool;
  /// @brief Counts the queued seat requests.
  ///
  /// @return The count.
  auto queued() const -> std::size_t;
  /// @brief Sends the queued passcode; called by the session loop.
  ///
  /// @return Nothing when none was queued or it was sent; the link's error
  ///     otherwise, which is logged.
  [[nodiscard]] auto flush_passcode()
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Sends queued seat requests, up to the queue depth; called by the
  /// session loop while streaming.
  ///
  /// A join drops the seat's queued controller states first; a removal
  /// restores its input. A refused join empties the seat.
  ///
  /// @return The refused joins.
  auto flush() -> std::vector<SeatRefusal>;
  /// @brief Drops every queued request and passcode and gives every seat's
  /// input back to the local player.
  void discard() noexcept;

  /// @brief Gives the wire controller type of a controller kind.
  ///
  /// @param controller The kind.
  /// @return 2 for DualShock 4, 6 for DualSense, 0 for the console's default.
  static auto wire(controllers::ControllerKind controller) noexcept
      -> std::uint8_t;

 private:
  /// @brief One seat as the session sees it.
  struct Held {
    /// @brief Where the seat stands.
    SeatState state{SeatState::empty};
    /// @brief Who holds it.
    SeatHolder holder{SeatHolder::local_player()};
    /// @brief The kind of controller in it.
    controllers::ControllerKind controller{
        controllers::ControllerKind::console_default};
  };

  /// @brief Reads a seat; call with the seats mutex held.
  ///
  /// @param seat The seat.
  /// @return The seat; an empty one when nobody holds it.
  auto held(controllers::SeatNumber seat) const -> Seat;
  /// @brief Marks a seat joining and queues its join; call with the seats
  /// mutex held.
  ///
  /// @param seat The seat.
  /// @param holder Who takes it.
  /// @param controller The kind of controller.
  /// @param as The PSN account the pad joins as.
  /// @return Nothing; InputRefusal::invalid for seat 0; InputRefusal::busy for
  ///     a held seat or a full queue; InputRefusal::not_running when commands
  ///     are not accepted.
  auto join(controllers::SeatNumber seat, SeatHolder holder,
            controllers::ControllerKind controller, std::optional<AccountId> as)
      -> Expected<void, InputRefusal>;
  /// @brief Gives a seat's input back to the local player.
  ///
  /// @param seat The seat; seat 0 is ignored.
  void release(controllers::SeatNumber seat);

  /// @brief Where the outcome of each sent request is logged.
  const CommandRoute& route_;
  /// @brief The controller queues.
  Controllers::Core& controllers_;
  /// @brief Guards the seats.
  mutable platform::Mutex mutex_;
  /// @brief The four seats.
  std::array<Held, controllers::SeatNumber::count> seats_{};
  /// @brief The seat requests and passcode waiting to be sent.
  SeatRequests requests_;
};

}  // namespace libchiaking::play
