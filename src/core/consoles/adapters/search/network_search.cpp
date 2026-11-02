#include "src/core/consoles/adapters/search/network_search.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

NetworkSearch::NetworkSearch(platform::NetworkProvider& network,
                             platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock,
                             SearchRanges ranges,
                             platform::DiagnosticsProvider* log,
                             Beacon::Settings beacon)
    : network_(network),
      threading_(threading),
      clock_(clock),
      ranges_(std::move(ranges)),
      settings_(std::move(beacon)),
      probes_(log),
      beacon_mutex_(threading),
      idle_(threading, clock, network) {}

NetworkSearch::~NetworkSearch() = default;

auto NetworkSearch::start() -> Expected<void, Failure> {
  if (beacon_ || cancelled()) return unexpected(Failure::invalid_state);
  if (!settings_.bind_address.is_ipv4())
    return unexpected(Failure::invalid_argument);
  auto settings = settings_;
  settings.targets.clear();
  settings.target_hosts.clear();
  settings.include_interface_broadcasts = false;
  auto beacon =
      std::make_unique<Beacon>(network_, threading_, clock_,
                               static_cast<Beacon::Watcher&>(*this), settings);
  if (const auto started = beacon->start(); !started)
    return unexpected(failure_of(started.error()));
  std::vector<Route> routes;
  const auto abandon = [&beacon, &routes] {
    (void)beacon->stop();
    for (auto& route : routes) (void)route.beacon->stop();
  };
  if (ranges_.unicast()) {
    for (const auto& subnet : ranges_.subnets()) {
      auto* network = ranges_.network_of(subnet).network().get();
      if (network == nullptr ||
          std::ranges::find(routes, network, &Route::network) != routes.end())
        continue;
      auto routed = std::make_unique<Beacon>(
          *network, threading_, clock_, static_cast<Beacon::Watcher&>(*this),
          settings);
      if (const auto started = routed->start(); !started) {
        abandon();
        return unexpected(failure_of(started.error()));
      }
      routes.push_back({network, std::move(routed)});
    }
  }
  {
    platform::Lock lock{beacon_mutex_};
    if (cancelled()) {
      abandon();
      return unexpected(Failure::invalid_state);
    }
    beacon_ = std::move(beacon);
    routes_ = std::move(routes);
  }
  plan_.emplace(ranges_, network_.capabilities().broadcast);
  for (const auto& address : pinned_) plan_->keep_probing(address);
  replies_.clear();
  next_interface_us_ = 0;
  permission_denied_ = false;
  return {};
}

void NetworkSearch::keep_probing(platform::IpAddress address) {
  pinned_.push_back(address);
  if (plan_) plan_->keep_probing(address);
}

auto NetworkSearch::refresh_interfaces(std::uint64_t now_us)
    -> Expected<void, Failure> {
  const auto listed = network_.interfaces().enumerate();
  if (!listed) return unexpected(Failure::network);
  if (!plan_->update_interfaces(listed.value()))
    return unexpected(Failure::invalid_argument);
  if (!probes_.started())
    probes_.start(now_us, plan_->interface_count(), plan_->progress().targets,
                  settings_.ps4_destination_port,
                  settings_.ps5_destination_port);
  next_interface_us_ = timing::Deadline::after_us(now_us, interface_refresh_us);
  return {};
}

auto NetworkSearch::probe(std::uint64_t now_us) -> Expected<void, Failure> {
  const auto destinations = plan_->destinations(now_us);
  if (destinations.empty()) return {};
  permission_denied_ = false;
  for (const auto& destination : destinations) {
    if (cancelled()) return unexpected(Failure::invalid_state);
    const auto sent = send_search_to(destination);
    if (!sent && sent.error() == Beacon::ProbeFailure::permission_denied) {
      permission_denied_ = true;
      break;
    }
  }
  return {};
}

auto NetworkSearch::send_search_to(platform::IpAddress destination)
    -> Expected<void, Beacon::ProbeFailure> {
  return beacon_for(destination).send_search_to(destination);
}

auto NetworkSearch::beacon_for(platform::IpAddress destination) -> Beacon& {
  const Subnet* narrowest = nullptr;
  for (const auto& subnet : ranges_.subnets()) {
    if (subnet.contains(destination) &&
        (narrowest == nullptr ||
         subnet.prefix_length() > narrowest->prefix_length()))
      narrowest = &subnet;
  }
  if (narrowest == nullptr) return *beacon_;
  const auto* network = ranges_.network_of(*narrowest).network().get();
  const auto route = std::ranges::find(routes_, network, &Route::network);
  return route == routes_.end() ? *beacon_ : *route->beacon;
}

auto NetworkSearch::receive(Beacon& beacon) -> Expected<void, Failure> {
  for (std::size_t count = 0; count < datagrams_per_pump; ++count) {
    if (cancelled()) return unexpected(Failure::invalid_state);
    const auto received = beacon.receive_once();
    if (!received) return unexpected(failure_of(received.error()));
    if (!received.value()) break;
  }
  return {};
}

auto NetworkSearch::pump_once() -> Expected<void, Failure> {
  if (!beacon_ || cancelled()) return unexpected(Failure::invalid_state);
  auto now = clock_.now_microseconds();
  if (now >= next_interface_us_) {
    if (const auto refreshed = refresh_interfaces(now); !refreshed)
      return refreshed;
    if (cancelled()) return unexpected(Failure::invalid_state);
    now = clock_.now_microseconds();
  }
  if (const auto probed = probe(now); !probed) return probed;
  if (const auto received = receive(*beacon_); !received) return received;
  for (auto& route : routes_)
    if (const auto received = receive(*route.beacon); !received)
      return received;
  now = clock_.now_microseconds();
  auto current = plan_->replies(now);
  if (probes_.silence_due(now)) probes_.silence(now, current.size());
  if (current != replies_) {
    replies_ = std::move(current);
    ++revision_;
  }
  return {};
}

auto NetworkSearch::poll(std::chrono::milliseconds timeout)
    -> Expected<void, Failure> {
  if (cancelled()) return unexpected(Failure::invalid_state);
  if (const auto pumped = pump_once(); !pumped) return pumped;
  wait(std::clamp(timeout, std::chrono::milliseconds{0}, maximum_poll));
  return pump_once();
}

auto NetworkSearch::stop() -> Expected<void, Failure> {
  if (!beacon_) return {};
  plan_.reset();
  platform::Lock lock{beacon_mutex_};
  auto stopped = beacon_->stop();
  beacon_.reset();
  for (auto& route : routes_)
    if (auto closed = route.beacon->stop(); !closed && stopped)
      stopped = std::move(closed);
  routes_.clear();
  if (!stopped) return unexpected(failure_of(stopped.error()));
  return {};
}

auto NetworkSearch::progress() const -> std::optional<SearchPlan::Progress> {
  if (!plan_) return std::nullopt;
  auto result = plan_->progress();
  result.permission_denied = permission_denied_;
  return result;
}

auto NetworkSearch::next_deadline_us(std::uint64_t now_us) const
    -> std::uint64_t {
  return std::min(next_interface_us_,
                  plan_->next_deadline_us(now_us).value_or(next_interface_us_));
}

auto NetworkSearch::next_deadline_microseconds() const -> std::uint64_t {
  return plan_ ? next_deadline_us(clock_.now_microseconds()) : 0;
}

auto NetworkSearch::local_endpoint() const
    -> Expected<platform::SocketEndpoint, Failure> {
  if (!beacon_) return unexpected(Failure::invalid_state);
  const auto endpoint = beacon_->local_endpoint();
  if (!endpoint) return unexpected(failure_of(endpoint.error()));
  return endpoint.value();
}

void NetworkSearch::wait(std::chrono::milliseconds timeout) {
  if (cancelled()) return;
  if (!beacon_) {
    idle_.wait(nullptr, platform::PollEvent::none, timeout);
    return;
  }
  const auto now = clock_.now_microseconds();
  const auto deadline = next_deadline_us(now);
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::microseconds{deadline > now ? deadline - now : 0} +
      std::chrono::microseconds{microseconds_per_millisecond - 1});
  const auto budget = std::min({timeout, longest_wait, remaining});
  if (routes_.empty()) {
    beacon_->wait(budget);
    return;
  }
  const auto share = budget / static_cast<std::int64_t>(routes_.size() + 1);
  beacon_->wait(share);
  for (auto& route : routes_) route.beacon->wait(share);
}

void NetworkSearch::wake() noexcept {
  idle_.wake();
  platform::Lock lock{beacon_mutex_};
  if (beacon_) beacon_->wake();
  for (auto& route : routes_) route.beacon->wake();
}

void NetworkSearch::cancel() noexcept {
  cancelled_.store(true, std::memory_order_release);
  wake();
}

void NetworkSearch::on_reply(SearchReply reply) {
  if (!cancelled()) plan_->receive(std::move(reply), clock_.now_microseconds());
}

void NetworkSearch::on_probe(platform::SocketEndpoint destination,
                             std::optional<Beacon::ProbeFailure> failure) {
  probes_.sent(destination, failure);
}

void NetworkSearch::on_datagram(bool admitted) { probes_.received(admitted); }

}  // namespace libchiaking::consoles
