#include "src/core/play/adapters/session/seats_core.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"

#include <string>
#include <utility>

namespace libchiaking::play {

static_assert(Seats::default_depth >= Seats::Core::request_depth);

Seats::Core::Core(platform::ThreadingProvider& threading,
                  const CommandRoute& route, Controllers::Core& controllers,
                  controllers::ControllerKind first_controller,
                  std::size_t depth)
    : route_(route),
      controllers_(controllers),
      mutex_(threading),
      requests_(threading, route, depth) {
  seats_[0].state = SeatState::joining;
  seats_[0].controller = first_controller;
}

auto Seats::Core::wire(controllers::ControllerKind controller) noexcept
    -> std::uint8_t {
  switch (controller) {
    case controllers::ControllerKind::console_default:
      return 0;
    case controllers::ControllerKind::dualshock4:
      return dualshock4_wire;
    case controllers::ControllerKind::dualsense:
      return dualsense_wire;
  }
  return 0;
}

auto Seats::Core::held(controllers::SeatNumber seat) const -> Seat {
  const auto& entry = seats_[seat.index()];
  return Seat::held(seat, entry.state, entry.holder, entry.controller)
      .value_or(Seat{seat});
}

auto Seats::Core::join(controllers::SeatNumber seat, SeatHolder holder,
                       controllers::ControllerKind controller,
                       std::optional<AccountId> as)
    -> Expected<void, InputRefusal> {
  if (!seat.is_added()) return unexpected(InputRefusal::invalid);
  const platform::Lock lock(mutex_);
  auto& entry = seats_[seat.index()];
  if (entry.state != SeatState::empty) return unexpected(InputRefusal::busy);
  if (const auto sent = requests_.enqueue(
          {SeatRequests::Request::Kind::join, seat, controller, as});
      !sent)
    return unexpected(sent.error());
  entry = {SeatState::joining, holder, controller};
  return {};
}

auto Seats::Core::add_player(controllers::SeatNumber seat,
                             controllers::ControllerKind controller,
                             std::optional<AccountId> as)
    -> Expected<void, InputRefusal> {
  return join(seat, SeatHolder::local_player(), controller, as);
}

auto Seats::Core::remove_player(controllers::SeatNumber seat)
    -> Expected<void, InputRefusal> {
  if (!seat.is_added()) return unexpected(InputRefusal::invalid);
  const platform::Lock lock(mutex_);
  auto& entry = seats_[seat.index()];
  if (entry.state != SeatState::empty && !entry.holder.is_local_player())
    return unexpected(InputRefusal::not_owner);
  if (const auto sent = requests_.enqueue(
          {SeatRequests::Request::Kind::remove, seat,
           controllers::ControllerKind::console_default, std::nullopt});
      !sent)
    return unexpected(sent.error());
  entry = {};
  return {};
}

auto Seats::Core::enter_passcode(controllers::SeatNumber seat, AccessCode code)
    -> Expected<void, InputRefusal> {
  return requests_.enter_passcode(seat, std::move(code));
}

auto Seats::Core::seat(controllers::SeatNumber seat) const -> Seat {
  const platform::Lock lock(mutex_);
  return held(seat);
}

auto Seats::Core::all() const
    -> std::array<Seat, controllers::SeatNumber::count> {
  const platform::Lock lock(mutex_);
  return {held(controllers::SeatNumber::masked(0)),
          held(controllers::SeatNumber::masked(1)),
          held(controllers::SeatNumber::masked(2)),
          held(controllers::SeatNumber::masked(3))};
}

auto Seats::Core::free_seat() const -> std::optional<controllers::SeatNumber> {
  const platform::Lock lock(mutex_);
  for (std::uint8_t index = 1; index < controllers::SeatNumber::count; ++index)
    if (seats_[index].state == SeatState::empty)
      return controllers::SeatNumber::masked(index);
  return std::nullopt;
}

auto Seats::Core::seat_guest(controllers::SeatNumber seat, GuestTag guest,
                             controllers::ControllerKind controller,
                             std::optional<AccountId> as)
    -> Expected<void, SeatRefusal> {
  if (!controllers_.divert(seat))
    return unexpected(SeatRefusal::no_free_seat());
  const auto joined =
      join(seat, SeatHolder::shared_guest(guest), controller, as);
  if (joined) return {};
  release(seat);
  if (joined.error() == InputRefusal::busy ||
      joined.error() == InputRefusal::invalid)
    return unexpected(SeatRefusal::no_free_seat());
  return unexpected(
      SeatRefusal::not_sent(seat, EndReason::Cause::invalid_state));
}

void Seats::Core::release_guest(controllers::SeatNumber seat) {
  {
    const platform::Lock lock(mutex_);
    auto& entry = seats_[seat.index()];
    if (entry.state == SeatState::empty || entry.holder.is_local_player())
      return;
    entry = {};
  }
  static_cast<void>(requests_.enqueue(
      {SeatRequests::Request::Kind::remove, seat,
       controllers::ControllerKind::console_default, std::nullopt}));
  release(seat);
}

void Seats::Core::session_is(SessionState state) {
  const platform::Lock lock(mutex_);
  if (state == SessionState::playing) {
    seats_[0].state = SeatState::playing;
    return;
  }
  if (state != SessionState::ended) return;
  for (auto& entry : seats_) entry.state = SeatState::empty;
}

auto Seats::Core::link_lost() -> std::vector<controllers::SeatNumber> {
  std::vector<controllers::SeatNumber> emptied;
  std::vector<controllers::SeatNumber> guests;
  {
    const platform::Lock lock(mutex_);
    seats_[0].state = SeatState::joining;
    for (std::uint8_t index = 1; index < controllers::SeatNumber::count;
         ++index) {
      auto& entry = seats_[index];
      if (entry.state == SeatState::empty) continue;
      const auto seat = controllers::SeatNumber::masked(index);
      if (!entry.holder.is_local_player()) guests.push_back(seat);
      entry = {};
      emptied.push_back(seat);
    }
  }
  for (const auto seat : guests) release(seat);
  return emptied;
}

void Seats::Core::passcode_needed(controllers::SeatNumber seat) {
  const platform::Lock lock(mutex_);
  seats_[seat.index()].state = SeatState::entering_passcode;
}

void Seats::Core::joined(controllers::SeatNumber seat) {
  const platform::Lock lock(mutex_);
  seats_[seat.index()].state = SeatState::playing;
}

void Seats::Core::emptied(controllers::SeatNumber seat) {
  if (!seat.is_added()) return;
  bool guest = false;
  {
    const platform::Lock lock(mutex_);
    auto& entry = seats_[seat.index()];
    guest = entry.state != SeatState::empty && !entry.holder.is_local_player();
    entry = {};
  }
  if (guest) release(seat);
}

auto Seats::Core::dropped(controllers::SeatNumber seat) -> bool {
  {
    const platform::Lock lock(mutex_);
    if (seat.is_added() && seats_[seat.index()].state == SeatState::joining)
      return false;
  }
  emptied(seat);
  return true;
}

void Seats::Core::release(controllers::SeatNumber seat) {
  if (seat.is_added()) controllers_.restore(seat);
}

auto Seats::Core::passcode_pending() const -> bool {
  return requests_.passcode_pending();
}

auto Seats::Core::queued() const -> std::size_t { return requests_.queued(); }

auto Seats::Core::flush_passcode()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return requests_.flush_passcode();
}

auto Seats::Core::flush() -> std::vector<SeatRefusal> {
  std::vector<SeatRefusal> refusals;
  for (std::size_t handled = 0; handled < requests_.depth(); ++handled) {
    auto request = requests_.next();
    if (!request) break;
    const bool removal = request->kind == SeatRequests::Request::Kind::remove;
    if (removal)
      controllers_.restore(request->seat);
    else
      controllers_.discard_pending(request->seat);
    const auto result = requests_.send(*request);
    const std::string seat = std::to_string(request->seat.index());
    if (result && removal) {
      route_.log(platform::LogSeverity::info,
                 "couch remove for pad " + seat + " was sent");
    } else if (result) {
      route_.log(platform::LogSeverity::info,
                 "couch join for pad " + seat + " was sent: controller type " +
                     std::to_string(wire(request->controller)));
    } else {
      route_.log(platform::LogSeverity::warning,
                 std::string{"couch "} + (removal ? "remove" : "join") +
                     " for pad " + seat + " was refused: result=" +
                     std::string{CommandRoute::result_name(result.error())});
      if (!removal) {
        emptied(request->seat);
        refusals.push_back(SeatRefusal::not_sent(
            request->seat, LinkEvidence::cause(result.error())));
      }
    }
  }
  return refusals;
}

void Seats::Core::discard() noexcept {
  controllers_.restore_all();
  requests_.clear();
}

auto Seats::add_player(controllers::SeatNumber seat,
                       controllers::ControllerKind controller,
                       std::optional<AccountId> as)
    -> Expected<void, InputRefusal> {
  return core_->add_player(seat, controller, as);
}

auto Seats::remove_player(controllers::SeatNumber seat)
    -> Expected<void, InputRefusal> {
  return core_->remove_player(seat);
}

auto Seats::enter_passcode(controllers::SeatNumber seat, AccessCode code)
    -> Expected<void, InputRefusal> {
  return core_->enter_passcode(seat, std::move(code));
}

auto Seats::seat(controllers::SeatNumber seat) const -> Seat {
  return core_->seat(seat);
}

auto Seats::all() const -> std::array<Seat, controllers::SeatNumber::count> {
  return core_->all();
}

}  // namespace libchiaking::play
