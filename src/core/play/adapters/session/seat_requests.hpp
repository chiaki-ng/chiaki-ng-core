/// @file
/// @brief Declares SeatRequests, the seat changes and passcode a session
/// queues for the console until its loop sends them.
#pragma once

#include "libchiaking/detail/controllers/controller_state.hpp"
#include "libchiaking/detail/controllers/seat_number.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/command_route.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace libchiaking::play {

/// @brief The seat changes waiting to be sent to the console, oldest first,
/// and the one passcode waiting beside them.
///
/// Any thread may queue; the session loop takes and sends. Queueing is
/// refused when the route does not accept commands or the queue is full, and
/// wakes the session thread once accepted. One mutex guards the queue and the
/// passcode; a caller holding the seats mutex may queue, never the reverse.
/// The route must outlive it. Not copyable.
class SeatRequests {
 public:
  /// @brief A seat change waiting to be sent.
  struct Request {
    /// @enum Kind
    /// @brief What to ask the console.
    ///
    /// @var Kind join
    /// @brief Admit a pad to the seat.
    ///
    /// @var Kind remove
    /// @brief Release the seat.
    enum class Kind : std::uint8_t { join, remove };

    /// @brief What to ask.
    Kind kind;
    /// @brief The seat.
    controllers::SeatNumber seat;
    /// @brief The kind of controller joining.
    controllers::ControllerKind controller{
        controllers::ControllerKind::console_default};
    /// @brief The PSN account the pad joins as; empty for a removal or when
    /// none is given.
    std::optional<AccountId> account;
  };

  /// @brief Builds an empty queue.
  ///
  /// @param threading Provider of the queue's mutex; must outlive this object.
  /// @param route Where requests go; must outlive this object.
  /// @param depth The most requests queued; at least 1.
  SeatRequests(platform::ThreadingProvider& threading,
               const CommandRoute& route, std::size_t depth);
  /// @brief Not copyable; the queue is one per session.
  SeatRequests(const SeatRequests&) = delete;
  /// @brief Not copy-assignable; the queue is one per session.
  auto operator=(const SeatRequests&) -> SeatRequests& = delete;

  /// @brief Reads the most requests queued, which is also the most one flush
  /// sends.
  ///
  /// @return The depth; at least 1.
  auto depth() const noexcept -> std::size_t { return depth_; }

  /// @brief Queues a request and wakes the session thread.
  ///
  /// @param request The request.
  /// @return Nothing; InputRefusal::invalid for seat 0;
  ///     InputRefusal::not_running when commands are not accepted;
  ///     InputRefusal::busy when the queue is full.
  [[nodiscard]] auto enqueue(Request request) -> Expected<void, InputRefusal>;
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
  /// @brief Takes the oldest queued request.
  ///
  /// @return The request; empty when none is queued.
  auto next() -> std::optional<Request>;
  /// @brief Sends one request through the link: a removal, or a join preceded
  /// by the account id when one is given.
  ///
  /// @param request The request.
  /// @return The link's result.
  [[nodiscard]] auto send(const Request& request) const
      -> Expected<void, net::protocol::remote_play::StreamError>;
  /// @brief Drops every queued request and the passcode.
  void clear() noexcept;

 private:
  /// @brief A passcode waiting to be sent.
  struct Passcode {
    /// @brief The seat it is for.
    controllers::SeatNumber seat;
    /// @brief The code.
    AccessCode code;
  };

  /// @brief Where requests go.
  const CommandRoute& route_;
  /// @brief The most requests queued.
  std::size_t depth_;
  /// @brief Guards the request queue and the passcode.
  mutable platform::Mutex mutex_;
  /// @brief Seat requests waiting to be sent, oldest first.
  std::deque<Request> requests_;
  /// @brief The passcode waiting to be sent.
  std::optional<Passcode> passcode_;
};

}  // namespace libchiaking::play
