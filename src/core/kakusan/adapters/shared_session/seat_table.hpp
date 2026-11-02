/// @file
/// @brief Declares SeatTable, the host's Kakusan seats: which the host
/// keeps for local players, which are reserved or granted to guests, and which
/// are free.
#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/short_block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace libchiaking::kakusan {

/// @brief What one Kakusan seat is being used for.
enum class SeatState : std::uint8_t {
  /// @brief Nobody holds it.
  free,
  /// @brief Kept for a player at the host.
  local,
  /// @brief Held for a verified guest while the console seats them.
  reserved,
  /// @brief Granted to a guest, who is getting ready or playing.
  granted,
};

/// @brief The seats of one shared session, numbered from 1 to the capacity.
///
/// The lowest seats start as local ones. Each reservation stamps its seat with
/// a new generation, so a binding (seat number and generation) names one
/// guest's hold on a seat and goes stale once the seat is released and taken
/// again. Changing the capacity or the number of local seats may take seats
/// back from guests: reservations first, then grants, highest seat first.
///
/// Owned by SharedSession::Core and used under its lock; not thread-safe.
class SeatTable {
 public:
  /// @brief Builds a table with the lowest `local_seats` seats local and the
  /// rest free.
  /// @param capacity The number of seats, at least 1.
  /// @param local_seats How many the host keeps, from 1 to `capacity`.
  /// @param last_generation The generation to count on from; zero for a new
  ///     session.
  /// @return The table.
  /// @retval net::kakusan::Error::internal `capacity` or `local_seats` is
  ///     zero.
  /// @retval net::kakusan::Error::no_capacity `local_seats` exceeds
  ///     `capacity`.
  [[nodiscard]] static auto create(std::uint8_t capacity,
                                   std::uint8_t local_seats,
                                   std::uint32_t last_generation = 0)
      -> net::kakusan::Result<SeatTable>;

  /// @brief Reserves the lowest free seat with a new generation.
  /// @return The binding of the reserved seat.
  /// @retval net::kakusan::Error::no_capacity No seat is free, or the
  ///     generations are used up.
  [[nodiscard]] auto reserve()
      -> net::kakusan::Result<net::kakusan::SeatBinding>;
  /// @brief Turns a reservation into a grant.
  /// @param binding The reservation.
  /// @return Nothing once granted.
  /// @retval net::kakusan::Error::protocol The binding is not a current
  ///     reservation.
  [[nodiscard]] auto grant(const net::kakusan::SeatBinding& binding)
      -> net::kakusan::Result<void>;
  /// @brief Frees a reserved or granted seat.
  /// @param binding The hold to release.
  /// @return False, changing nothing, when the binding is stale or unknown.
  auto release(const net::kakusan::SeatBinding& binding) -> bool;

  /// @brief What a guest's hold currently is.
  /// @param binding The hold.
  /// @return SeatState::reserved or SeatState::granted for a current hold,
  ///     otherwise SeatState::free.
  auto state_of(const net::kakusan::SeatBinding& binding) const noexcept
      -> SeatState;
  /// @brief Tells whether a binding is a current reservation or grant.
  /// @param binding The hold.
  /// @return True when it is.
  auto holds(const net::kakusan::SeatBinding& binding) const noexcept -> bool;
  /// @brief Tells whether a binding is a current grant.
  /// @param binding The hold.
  /// @return True when it is.
  auto owns(const net::kakusan::SeatBinding& binding) const noexcept -> bool;
  /// @brief Tells whether a seat is kept for the host.
  /// @param seat The seat number.
  /// @return True when it is within the capacity and local.
  auto is_local(std::uint8_t seat) const noexcept -> bool;

  /// @brief Changes how many seats the host keeps.
  ///
  /// Growing takes the lowest free seats, revoking guests' seats when not
  /// enough are free. Shrinking frees local seats from the top, always
  /// keeping seat 1.
  ///
  /// @param local_seats The new number, from 1 to the capacity.
  /// @return The bindings taken back from guests, already freed.
  /// @retval net::kakusan::Error::internal `local_seats` is zero.
  /// @retval net::kakusan::Error::no_capacity It exceeds the capacity.
  [[nodiscard]] auto set_local_seats(std::uint8_t local_seats)
      -> net::kakusan::Result<std::vector<net::kakusan::SeatBinding>>;
  /// @brief Changes the number of seats.
  ///
  /// Shrinking takes back every guest seat above the new capacity and moves
  /// local seats from there to the lowest free seats below it, revoking more
  /// guests when not enough are free.
  ///
  /// @param capacity The new number of seats, at least the local seat count.
  /// @return The bindings taken back from guests, already freed.
  /// @retval net::kakusan::Error::internal `capacity` is zero.
  /// @retval net::kakusan::Error::no_capacity It is below the local seat
  ///     count.
  [[nodiscard]] auto set_capacity(std::uint8_t capacity)
      -> net::kakusan::Result<std::vector<net::kakusan::SeatBinding>>;

  /// @brief The seat counts.
  /// @return Capacity, local, granted (remote) and reserved counts.
  auto summary() const noexcept -> SeatSummary;

  /// @brief The number of seats.
  /// @return The capacity.
  auto capacity() const noexcept -> std::uint8_t { return capacity_; }
  /// @brief The number of seats the host keeps.
  /// @return The local seat count.
  auto local_seats() const noexcept -> std::uint8_t { return local_; }
  /// @brief The number of seats granted to guests.
  /// @return The granted count.
  auto remote_seats() const noexcept -> std::uint8_t { return granted_; }
  /// @brief The number of seats reserved for guests being seated.
  /// @return The reserved count.
  auto reserved_seats() const noexcept -> std::uint8_t { return reserved_; }
  /// @brief The generation of the latest reservation.
  /// @return The generation; zero before the first.
  auto last_generation() const noexcept -> std::uint32_t {
    return last_generation_;
  }

 private:
  /// @brief One seat.
  struct Slot {
    /// @brief What it is used for.
    SeatState state{SeatState::free};
    /// @brief The generation of its latest reservation; meaningful while
    /// reserved or granted.
    std::uint32_t generation{};
  };

  /// @brief Builds the table from checked arguments; see create().
  /// @param capacity The number of seats.
  /// @param local_seats How many the host keeps.
  /// @param last_generation The generation to count on from.
  SeatTable(std::uint8_t capacity, std::uint8_t local_seats,
            std::uint32_t last_generation) noexcept;

  /// @brief Finds the slot a binding currently holds.
  /// @param binding The hold.
  /// @return The slot when the binding is a current reservation or grant,
  ///     otherwise null.
  auto find(const net::kakusan::SeatBinding& binding) noexcept -> Slot*;
  /// @brief Finds the slot a binding currently holds.
  /// @param binding The hold.
  /// @return The slot when the binding is a current reservation or grant,
  ///     otherwise null.
  auto find(const net::kakusan::SeatBinding& binding) const noexcept
      -> const Slot*;
  /// @brief A range of seats and how many guest seats to take back from it.
  struct Span {
    /// @brief The first seat.
    unsigned low{};
    /// @brief The last seat; below `low` for an empty range.
    unsigned high{};
    /// @brief Most seats to take.
    std::size_t limit{};
  };

  /// @brief Picks guest seats to take back: reservations before grants,
  /// highest seat first within each span, up to each span's limit.
  /// @param spans The seat ranges and how many to take from each.
  /// @return The chosen bindings; nothing is freed yet.
  auto collect(const std::array<Span, 2>& spans) const
      -> std::vector<net::kakusan::SeatBinding>;
  /// @brief Counts the seats in a range in one state.
  /// @param low The first seat.
  /// @param high The last seat; a range with `high` below `low` is empty.
  /// @param state The state to count.
  /// @return The count.
  auto count_in(unsigned low, unsigned high, SeatState state) const noexcept
      -> std::size_t;
  /// @brief Frees a seat, keeping the reserved and granted counts right.
  /// @param seat The seat number.
  void clear(std::size_t seat) noexcept;
  /// @brief Makes the lowest free seats local.
  /// @param count How many; fewer when fewer are free.
  void convert_lowest_free(std::size_t count) noexcept;

  /// @brief The seats, indexed by seat number; index 0 is unused.
  std::array<Slot, net::kakusan::max_seat_capacity + 1> slots_{};
  /// @brief The generation of the latest reservation.
  std::uint32_t last_generation_{};
  /// @brief The number of seats.
  std::uint8_t capacity_{};
  /// @brief The number of local seats.
  std::uint8_t local_{};
  /// @brief The number of granted seats.
  std::uint8_t granted_{};
  /// @brief The number of reserved seats.
  std::uint8_t reserved_{};
};

}  // namespace libchiaking::kakusan
