/// @file
/// @brief Declares the seats of a session: the up to four local players and how
/// further ones are added and removed.
#pragma once

#include "libchiaking/detail/controllers/controller_feedback.hpp"
#include "libchiaking/detail/controllers/controller_state.hpp"
#include "libchiaking/detail/controllers/seat_number.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/access_code.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief What is happening in a seat.
enum class SeatState : std::uint8_t {
  /// @brief Nobody holds the seat.
  empty,
  /// @brief The seat was asked for and the session has not yet been told it
  /// joined. Seat 0 reads like this until the stream is running.
  joining,
  /// @brief A PS5 console is waiting for the passcode of the seat's account.
  entering_passcode,
  /// @brief The seat is in the game. For seat 0 this means the stream is
  /// running; for an added seat, that SessionEvents::on_seat_joined was
  /// reported.
  playing,
};

/// @brief A number the sharing frontend chooses to tell one remote guest from
/// another.
///
/// The library only stores and returns it.
class GuestTag {
 public:
  /// @brief Wraps the frontend's number for a guest.
  ///
  /// @param value Any value; its meaning belongs to the frontend.
  explicit constexpr GuestTag(std::uint64_t value) noexcept : value_(value) {}

  /// @brief The number given at construction.
  ///
  /// @return The number.
  constexpr auto value() const noexcept -> std::uint64_t { return value_; }

  /// @brief Compares the wrapped numbers.
  friend constexpr auto operator==(const GuestTag&, const GuestTag&) noexcept
      -> bool = default;
  /// @brief Orders tags by their numbers.
  friend constexpr auto operator<=>(const GuestTag&, const GuestTag&) noexcept
      -> std::strong_ordering = default;

 private:
  std::uint64_t value_;
};

/// @brief Who holds a seat: someone at this device, or a guest the session is
/// shared with.
class SeatHolder {
 public:
  /// @brief Makes the holder for a player at this device.
  ///
  /// @return The holder.
  static constexpr auto local_player() noexcept -> SeatHolder {
    return SeatHolder{std::nullopt};
  }
  /// @brief Makes the holder for a shared guest.
  ///
  /// @param guest The guest's tag.
  ///
  /// @return The holder.
  static constexpr auto shared_guest(GuestTag guest) noexcept -> SeatHolder {
    return SeatHolder{guest};
  }

  /// @brief Reports whether the holder is a player at this device.
  ///
  /// @return `true` for a player at this device.
  constexpr auto is_local_player() const noexcept -> bool {
    return !guest_.has_value();
  }
  /// @brief The guest's tag, or nothing when the holder is a player at this
  /// device.
  ///
  /// @return The tag, or nothing.
  constexpr auto guest() const noexcept -> std::optional<GuestTag> {
    return guest_;
  }

  /// @brief Compares the kind of holder and, for guests, the tag.
  friend constexpr auto operator==(const SeatHolder&,
                                   const SeatHolder&) noexcept
      -> bool = default;

 private:
  constexpr explicit SeatHolder(std::optional<GuestTag> guest) noexcept
      : guest_(guest) {}

  std::optional<GuestTag> guest_;
};

/// @brief A snapshot of one seat: its state, who holds it and with which
/// controller.
///
/// Values of this type are copies taken by Seats::seat or Seats::all; they do
/// not change when the seat does.
class Seat {
 public:
  /// @brief Makes the snapshot of an empty seat.
  ///
  /// @param number The seat described.
  constexpr explicit Seat(controllers::SeatNumber number) noexcept
      : number_(number) {}

  /// @brief Makes the snapshot of an occupied seat.
  ///
  /// @param number The seat described.
  /// @param state The seat's state; must not be SeatState::empty.
  /// @param holder Who holds the seat.
  /// @param controller The controller model the seat was asked for with.
  /// @return The snapshot, or nothing when state is SeatState::empty.
  [[nodiscard]] static constexpr auto held(
      controllers::SeatNumber number, SeatState state, SeatHolder holder,
      controllers::ControllerKind controller) noexcept -> std::optional<Seat> {
    if (state == SeatState::empty) return std::nullopt;
    Seat seat{number};
    seat.state_ = state;
    seat.holder_ = holder;
    seat.controller_ = controller;
    return seat;
  }

  /// @brief The seat this snapshot describes.
  ///
  /// @return The seat.
  constexpr auto number() const noexcept -> controllers::SeatNumber {
    return number_;
  }
  /// @brief The seat's state when the snapshot was taken.
  ///
  /// @return The state.
  constexpr auto state() const noexcept -> SeatState { return state_; }
  /// @brief Reports whether nobody held the seat.
  ///
  /// @return `true` when the seat was empty.
  constexpr auto is_empty() const noexcept -> bool {
    return state_ == SeatState::empty;
  }
  /// @brief Who held the seat, or nothing when it was empty.
  ///
  /// @return The holder, or nothing.
  constexpr auto holder() const noexcept -> std::optional<SeatHolder> {
    return holder_;
  }
  /// @brief The controller model the seat was asked for with, or nothing when
  /// it was empty.
  ///
  /// @return The controller model, or nothing.
  constexpr auto controller() const noexcept
      -> std::optional<controllers::ControllerKind> {
    return controller_;
  }
  /// @brief Reports whether the seat was held by a shared guest rather than a
  /// player at this device.
  ///
  /// @return `true` for a shared guest.
  constexpr auto is_held_by_guest() const noexcept -> bool {
    return holder_ && !holder_->is_local_player();
  }

  /// @brief Compares every part of the snapshot.
  friend constexpr auto operator==(const Seat&, const Seat&) noexcept
      -> bool = default;

 private:
  controllers::SeatNumber number_;
  SeatState state_{SeatState::empty};
  std::optional<SeatHolder> holder_{};
  std::optional<controllers::ControllerKind> controller_{};
};

/// @brief Why a request to fill a seat failed.
///
/// Reported through SessionEvents::on_seat_refused and
/// SharedAudience::guest_seat_refused, and returned by
/// SessionShare::seat_guest.
class SeatRefusal {
 public:
  /// @brief The kind of failure.
  enum class Kind : std::uint8_t {
    /// @brief The request could not be sent to the console; cause() says
    /// why. On PS5 this includes a join with no account, with an account
    /// already in another seat, or with an account the console could not
    /// resolve earlier.
    not_sent,
    /// @brief The console answered with a failure status; console_status()
    /// holds it.
    refused_by_console,
    /// @brief The console asked for the passcode again after three
    /// attempts.
    passcode_attempts_used,
    /// @brief A PS5 console did not answer the join within ten seconds.
    timed_out,
    /// @brief The console has no user for the account the seat asked to
    /// join as.
    account_not_on_console,
    /// @brief No added seat is free. Only SessionShare::seat_guest reports
    /// this, and no seat is named.
    no_free_seat,
  };

  /// @brief The console status that by_console reads as
  /// Kind::passcode_attempts_used.
  static constexpr std::uint8_t passcode_attempts_status = 0xfe;
  /// @brief The console status that by_console reads as Kind::timed_out.
  static constexpr std::uint8_t timed_out_status = 0xff;

  /// @brief Makes a refusal for a request that never reached the console.
  ///
  /// @param seat The seat asked for.
  /// @param cause Why the request could not be sent.
  ///
  /// @return The Kind::not_sent refusal.
  static constexpr auto not_sent(controllers::SeatNumber seat,
                                 EndReason::Cause cause) noexcept
      -> SeatRefusal {
    SeatRefusal refusal{Kind::not_sent, seat};
    refusal.cause_ = cause;
    return refusal;
  }

  /// @brief Makes a refusal from the status a console returned.
  ///
  /// @param seat The seat asked for.
  /// @param status The console's status byte. passcode_attempts_status and
  ///     timed_out_status become their own kinds; any other value becomes
  ///     Kind::refused_by_console.
  ///
  /// @return The refusal the status calls for.
  static constexpr auto by_console(controllers::SeatNumber seat,
                                   std::uint8_t status) noexcept
      -> SeatRefusal {
    if (status == passcode_attempts_status)
      return SeatRefusal{Kind::passcode_attempts_used, seat};
    if (status == timed_out_status) return SeatRefusal{Kind::timed_out, seat};
    SeatRefusal refusal{Kind::refused_by_console, seat};
    refusal.console_status_ = status;
    return refusal;
  }

  /// @brief Makes a refusal for an account the console does not know.
  ///
  /// @param seat The seat asked for.
  ///
  /// @return The Kind::account_not_on_console refusal.
  static constexpr auto account_not_on_console(
      controllers::SeatNumber seat) noexcept -> SeatRefusal {
    return SeatRefusal{Kind::account_not_on_console, seat};
  }

  /// @brief Makes the refusal for a guest when every added seat is taken.
  ///
  /// @return The Kind::no_free_seat refusal.
  static constexpr auto no_free_seat() noexcept -> SeatRefusal {
    return SeatRefusal{Kind::no_free_seat, std::nullopt};
  }

  /// @brief The kind of failure.
  ///
  /// @return The kind.
  constexpr auto kind() const noexcept -> Kind { return kind_; }
  /// @brief The seat the refusal is about; nothing for Kind::no_free_seat.
  ///
  /// @return The seat, or nothing.
  constexpr auto seat() const noexcept
      -> std::optional<controllers::SeatNumber> {
    return seat_;
  }
  /// @brief Why the request could not be sent; present only for
  /// Kind::not_sent.
  ///
  /// @return The cause, or nothing.
  constexpr auto cause() const noexcept -> std::optional<EndReason::Cause> {
    return cause_;
  }
  /// @brief The console's status byte; present only for
  /// Kind::refused_by_console.
  ///
  /// @return The status byte, or nothing.
  constexpr auto console_status() const noexcept
      -> std::optional<std::uint8_t> {
    return console_status_;
  }

  /// @brief Compares every part of the refusal.
  friend constexpr auto operator==(const SeatRefusal&,
                                   const SeatRefusal&) noexcept
      -> bool = default;

 private:
  constexpr SeatRefusal(Kind kind,
                        std::optional<controllers::SeatNumber> seat) noexcept
      : kind_(kind), seat_(seat) {}

  Kind kind_;
  std::optional<controllers::SeatNumber> seat_;
  std::optional<EndReason::Cause> cause_{};
  std::optional<std::uint8_t> console_status_{};
};

/// @brief Adds and removes the further local players of a session and reports
/// who holds each seat.
///
/// Seat 0 belongs to the first player for the whole session and can be neither
/// added nor removed. Seats 1 to 3 start empty.
///
/// add_player, remove_player and enter_passcode only queue a request and
/// return; the session's thread sends it once the stream is running. What the
/// console makes of a join arrives later through SessionEvents:
/// on_seat_passcode_needed, on_seat_joined, on_seat_refused and on_seat_left.
///
/// The handshake differs by route. A PS5 console signs the seat in as an
/// account, so add_player needs one, and the console may ask for that account's
/// passcode. A PS4 console needs no account. A cloud session needs no account
/// and has no console handshake; the seat's controller is opened directly on
/// the stream.
///
/// When an established stream is lost every added seat is emptied, and players
/// have to be added again after the session has reconnected.
///
/// Obtained from Session::seats and valid for as long as that Session. All
/// members may be called from any thread.
///
/// @note A PS5 console's acceptance of a seat is delivered to
///     MediaSink::on_feedback as SeatConfirmed, carrying the seat's light
///     colour.
///
/// @see SessionEvents
/// @see Controllers
class Seats {
 public:
  /// @brief Implementation behind the seats; owned by the session.
  class Core;

  /// @brief Number of seat requests a session queues before refusing more
  /// with InputRefusal::busy.
  static constexpr std::size_t default_depth = 16;

  /// @brief Binds the seats to the session's implementation.
  ///
  /// Frontends do not construct this; they use Session::seats.
  ///
  /// @param core Implementation owned by the session. It must outlive this
  ///     object.
  explicit Seats(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; a session has one set of seats.
  Seats(const Seats&) = delete;
  /// @brief Not copy-assignable; a session has one set of seats.
  auto operator=(const Seats&) -> Seats& = delete;

  /// @brief Asks for a further local player to join in an added seat.
  ///
  /// On success the seat reads as SeatState::joining at once. The request is
  /// sent once the stream is running, and controller input still queued for
  /// the seat at that moment is discarded. The outcome arrives through
  /// SessionEvents.
  ///
  /// @param seat The seat to fill. Seat 0 is the first player and is refused.
  /// @param controller The controller model to announce for the seat.
  /// @param as Account to sign the seat in as. Required by a PS5 console,
  ///     where each seat needs a different account; not needed on PS4 or by a
  ///     cloud session.
  /// @return Nothing on success. The request is only queued.
  /// @retval InputRefusal::invalid Seat 0 was named.
  /// @retval InputRefusal::busy The seat is not empty, or too many seat
  ///     requests are waiting.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  /// @see SessionEvents::on_seat_joined
  /// @see SessionEvents::on_seat_refused
  /// @see SessionEvents::on_seat_passcode_needed
  [[nodiscard]] auto add_player(controllers::SeatNumber seat,
                                controllers::ControllerKind controller,
                                std::optional<AccountId> as = std::nullopt)
      -> Expected<void, InputRefusal>;
  /// @brief Empties an added seat held by a player at this device.
  ///
  /// On success the seat reads as SeatState::empty at once, also when its
  /// join was still under way. The console is told once the stream is
  /// running. Removing a seat that is already empty is accepted.
  ///
  /// @param seat The seat to empty. Seat 0 is the first player and is
  ///     refused.
  /// @return Nothing on success. The request is only queued.
  /// @retval InputRefusal::invalid Seat 0 was named.
  /// @retval InputRefusal::not_owner The seat is held by a shared guest; it
  ///     is released by destroying that guest's SharedSeat.
  /// @retval InputRefusal::busy Too many seat requests are waiting.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto remove_player(controllers::SeatNumber seat)
      -> Expected<void, InputRefusal>;
  /// @brief Answers a PS5 console's request for the passcode of the account
  /// joining a seat.
  ///
  /// Call it after SessionEvents::on_seat_passcode_needed. The code is sent
  /// by the session's thread; a code given before an earlier one was sent
  /// replaces it. A wrong code makes the console ask again, and after three
  /// attempts the join fails with SeatRefusal::Kind::passcode_attempts_used.
  ///
  /// @param seat The seat the console asked about.
  /// @param code The passcode. It is moved into the request and kept only
  ///     until it is sent.
  /// @return Nothing on success. The code is only queued; whether it was
  ///     right is reported through SessionEvents.
  /// @retval InputRefusal::invalid The code is not a well-formed passcode.
  /// @retval InputRefusal::not_running The session is not accepting requests.
  [[nodiscard]] auto enter_passcode(controllers::SeatNumber seat,
                                    AccessCode code)
      -> Expected<void, InputRefusal>;
  /// @brief Returns a snapshot of one seat.
  ///
  /// @param seat The seat to describe.
  /// @return The seat as it is now; the copy does not follow later changes.
  auto seat(controllers::SeatNumber seat) const -> Seat;
  /// @brief Returns a snapshot of all four seats, indexed by seat number.
  ///
  /// @return The four seats.
  auto all() const -> std::array<Seat, controllers::SeatNumber::count>;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
