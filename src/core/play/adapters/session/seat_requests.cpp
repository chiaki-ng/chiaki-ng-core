#include "src/core/play/adapters/session/seat_requests.hpp"

#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::play {

SeatRequests::SeatRequests(platform::ThreadingProvider& threading,
                           const CommandRoute& route, std::size_t depth)
    : route_(route),
      depth_(std::max<std::size_t>(depth, 1)),
      mutex_(threading) {}

auto SeatRequests::enqueue(Request request) -> Expected<void, InputRefusal> {
  if (!request.seat.is_added()) return unexpected(InputRefusal::invalid);
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.refusal()) return unexpected(*refusal);
    if (requests_.size() >= depth_) return unexpected(InputRefusal::busy);
    requests_.push_back(std::move(request));
  }
  route_.wake();
  return {};
}

auto SeatRequests::enter_passcode(controllers::SeatNumber seat, AccessCode code)
    -> Expected<void, InputRefusal> {
  auto wired = AccessCode::parse(code.text());
  if (!wired) return unexpected(InputRefusal::invalid);
  {
    const platform::Lock lock(mutex_);
    if (const auto refusal = route_.refusal()) return unexpected(*refusal);
    passcode_.emplace(Passcode{seat, std::move(*wired)});
  }
  route_.wake();
  return {};
}

auto SeatRequests::passcode_pending() const -> bool {
  const platform::Lock lock(mutex_);
  return passcode_.has_value();
}

auto SeatRequests::queued() const -> std::size_t {
  const platform::Lock lock(mutex_);
  return requests_.size();
}

auto SeatRequests::flush_passcode()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  std::optional<Passcode> pending;
  {
    const platform::Lock lock(mutex_);
    pending = std::exchange(passcode_, std::nullopt);
  }
  if (!pending) return {};
  const auto result = route_.driver().execute(
      net::protocol::remote_play::command::CouchPasscode{
          pending->seat.index(), std::move(pending->code)});
  if (!result)
    route_.log(platform::LogSeverity::warning,
               "controller passcode for pad " +
                   std::to_string(pending->seat.index()) +
                   " was not sent: result=" +
                   std::string{CommandRoute::result_name(result.error())});
  return result;
}

auto SeatRequests::next() -> std::optional<Request> {
  const platform::Lock lock(mutex_);
  if (requests_.empty()) return std::nullopt;
  std::optional<Request> request = std::move(requests_.front());
  requests_.pop_front();
  return request;
}

auto SeatRequests::send(const Request& request) const
    -> Expected<void, net::protocol::remote_play::StreamError> {
  auto& driver = route_.driver();
  const auto seat = request.seat.index();
  if (request.kind == Request::Kind::remove)
    return driver.execute(
        net::protocol::remote_play::command::CouchRemove{seat});
  if (request.account) {
    if (const auto set = driver.execute(
            net::protocol::remote_play::command::CouchSetAccountId{
                seat, *request.account});
        !set)
      return set;
  }
  return driver.execute(net::protocol::remote_play::command::CouchJoin{
      seat, Seats::Core::wire(request.controller)});
}

void SeatRequests::clear() noexcept {
  const platform::Lock lock(mutex_);
  requests_.clear();
  passcode_.reset();
}

}  // namespace libchiaking::play
