#include "src/core/consoles/usecases/find.hpp"

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/consoles/adapters/roster/roster.hpp"
#include "src/core/consoles/adapters/search/beacon.hpp"
#include "src/core/consoles/model/search/search_reply.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::consoles::usecases {

class Find::Lookout final : public Beacon::Watcher {
 public:
  explicit Lookout(HardwareId wanted) noexcept : wanted_(wanted) {}

  void on_reply(SearchReply reply) override {
    if (reply.hardware_id() == wanted_) seen_ = std::move(reply);
  }

  auto take() noexcept -> std::optional<SearchReply> {
    return std::exchange(seen_, std::nullopt);
  }

 private:
  HardwareId wanted_;
  std::optional<SearchReply> seen_;
};

Find::Find(platform::NetworkProvider& network,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock, SharedSearch& search,
           Roster& roster) noexcept
    : network_(network),
      threading_(threading),
      clock_(clock),
      search_(search),
      roster_(roster),
      facts_(roster) {}

auto Find::on_my_network(const SearchRanges& ranges, NetworkWatcher& watcher,
                         const platform::StopToken& stop)
    -> Expected<void, SearchFailure> {
  auto joined = search_.join(ranges);
  if (!joined) return unexpected(joined.error());
  SharedSearch::Seat seat{search_, std::move(joined.value())};
  while (!stop.stop_requested()) {
    const auto turn = seat.turn(
        [this](const NetworkSearch& search) { return nearby(search); });
    if (turn.fresh) watcher.on_view(*turn.fresh);
    if (!turn.stepped) return turn.stepped;
  }
  return {};
}

auto Find::nearby(const NetworkSearch& search)
    -> std::vector<std::shared_ptr<Console>> {
  std::vector<std::shared_ptr<Console>> nearby;
  for (const auto& reply : search.replies()) {
    if (auto sighting = reply.sighting())
      nearby.push_back(facts_.sighted(std::move(*sighting)));
  }
  for (const auto& console : roster_.everything()) {
    if (std::ranges::find(nearby, console) == nearby.end())
      facts_.out_of_sight(*console, nearby.empty());
  }
  return nearby;
}

auto Find::check(Console& console, const platform::StopToken& stop,
                 HomeNetwork over) -> Expected<Presence, Unreachable> {
  if (console.is_forgotten()) return unexpected(Unreachable::forgotten);
  const auto address = console.last_address();
  if (!address) return unexpected(Unreachable::no_address);
  Lookout lookout{console.hardware_id()};
  Beacon::Settings settings;
  settings.target_hosts = {std::string{address->unbracketed()}};
  settings.include_interface_broadcasts = false;
  if (address->is_ipv6())
    settings.bind_address = platform::IpAddress::any_ipv6();
  Beacon beacon{over.provider_or(network_), threading_, clock_, lookout,
                std::move(settings)};
  if (!beacon.start() || !beacon.send_search())
    return unexpected(Unreachable::network);
  const auto deadline =
      timing::Deadline::after(clock_, Console::check_deadline);
  for (;;) {
    if (stop.stop_requested()) return unexpected(Unreachable::cancelled);
    const auto now = timing::Deadline::now(clock_);
    if (deadline.expired(now)) break;
    beacon.wait(std::min(Console::check_wait_step,
                         std::chrono::ceil<std::chrono::milliseconds>(
                             *deadline.remaining(now))));
    if (stop.stop_requested()) return unexpected(Unreachable::cancelled);
    for (std::size_t count = 0; count < Beacon::datagrams_per_pump; ++count) {
      const auto received = beacon.receive_once();
      if (!received) return unexpected(Unreachable::network);
      if (!received.value()) break;
    }
    if (const auto reply = lookout.take()) {
      facts_.recognised(console, reply->sighting());
      return reply->in_standby() ? Presence::rest_mode : Presence::ready;
    }
  }
  facts_.out_of_sight(console, false);
  return unexpected(Unreachable::timed_out);
}

}  // namespace libchiaking::consoles::usecases
