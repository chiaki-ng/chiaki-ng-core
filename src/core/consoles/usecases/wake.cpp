#include "src/core/consoles/usecases/wake.hpp"

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/consoles/adapters/search/beacon.hpp"
#include "src/core/consoles/adapters/search/network_search.hpp"
#include "src/core/consoles/model/console/wake_credential.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace libchiaking::consoles::usecases {

Wake::Wake(platform::NetworkProvider& network,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock, Roster& roster) noexcept
    : network_(network), threading_(threading), clock_(clock), facts_(roster) {}

auto Wake::sight(Console& console, const SearchReply& reply) -> Presence {
  facts_.recognised(console, reply.sighting());
  return reply.in_standby() ? Presence::rest_mode : Presence::ready;
}

auto Wake::at(platform::NetworkProvider& network, const Host& address,
              Generation generation, const PairingKeys& keys)
    -> Expected<void, WakeRefusal> {
  const auto credential =
      WakeCredential::from_registration_key(keys.registration_key());
  if (!credential) return unexpected(WakeRefusal::not_paired);
  if (!Beacon::wake_host(network, threading_, clock_, address.unbracketed(), 0,
                         generation, *credential))
    return unexpected(WakeRefusal::network);
  return {};
}

auto Wake::now(Console& console, const users::PsUser& as, HomeNetwork over)
    -> Expected<void, WakeRefusal> {
  if (console.is_forgotten()) return unexpected(WakeRefusal::forgotten);
  const auto keys = facts_.keys_of(console, as);
  if (!keys) return unexpected(WakeRefusal::not_paired);
  const auto address = console.last_address();
  if (!address) return unexpected(WakeRefusal::no_address);
  return at(over.provider_or(network_), *address, console.generation(), *keys);
}

auto Wake::and_wait(Console& console, const users::PsUser& as,
                    const platform::StopToken& stop, HomeNetwork over)
    -> Expected<void, WakeRefusal> {
  if (console.is_forgotten()) return unexpected(WakeRefusal::forgotten);
  const auto keys = facts_.keys_of(console, as);
  if (!keys || !WakeCredential::from_registration_key(keys->registration_key()))
    return unexpected(WakeRefusal::not_paired);
  auto& network = over.provider_or(network_);
  const auto known = console.last_address();
  const bool searchable = known && !known->is_ipv6();
  if (!searchable && !network.capabilities().broadcast)
    return unexpected(WakeRefusal::no_address);
  NetworkSearch search{network, threading_, clock_, SearchRanges{}};
  if (searchable) {
    if (const auto resolved = network.dns().resolve(known->unbracketed(), {})) {
      for (const auto& candidate : resolved.value())
        search.keep_probing(candidate);
    }
  }
  if (!search.start()) return unexpected(WakeRefusal::network);
  const auto mine = [&search, &console]() -> std::optional<SearchReply> {
    const auto replies = search.replies();
    const auto found = std::ranges::find(replies, console.hardware_id(),
                                         &SearchReply::hardware_id);
    if (found == replies.end()) return std::nullopt;
    return *found;
  };
  const auto waited =
      [&](std::chrono::seconds limit,
          bool until_ready) -> Expected<SearchReply, WakeRefusal> {
    const auto deadline = timing::Deadline::after(clock_, limit);
    for (;;) {
      if (stop.stop_requested()) return unexpected(WakeRefusal::cancelled);
      if (!search.poll()) return unexpected(WakeRefusal::network);
      if (search.progress().value_or(SearchPlan::Progress{}).permission_denied)
        return unexpected(WakeRefusal::network);
      if (auto reply = mine(); reply && !(until_ready && reply->in_standby()))
        return std::move(*reply);
      if (deadline.expired(clock_))
        return unexpected(until_ready ? WakeRefusal::still_in_rest_mode
                                      : WakeRefusal::not_found);
    }
  };
  const auto found = waited(Console::find_deadline, false);
  if (!found) {
    if (found.error() == WakeRefusal::not_found)
      facts_.out_of_sight(console, false);
    return unexpected(found.error());
  }
  if (sight(console, found.value()) == Presence::ready) return {};
  const auto address = console.last_address();
  if (!address) return unexpected(WakeRefusal::no_address);
  if (const auto woken = at(network, *address, console.generation(), *keys);
      !woken)
    return woken;
  auto resend = timing::Deadline::after(clock_, Console::wake_resend_interval);
  const auto deadline =
      timing::Deadline::after(clock_, Console::ready_deadline);
  for (;;) {
    if (stop.stop_requested()) return unexpected(WakeRefusal::cancelled);
    if (!search.poll()) return unexpected(WakeRefusal::network);
    if (search.progress().value_or(SearchPlan::Progress{}).permission_denied)
      return unexpected(WakeRefusal::network);
    if (auto reply = mine(); reply && !reply->in_standby()) {
      sight(console, *reply);
      return {};
    }
    if (deadline.expired(clock_))
      return unexpected(WakeRefusal::still_in_rest_mode);
    if (resend.expired(clock_)) {
      static_cast<void>(at(network, *address, console.generation(), *keys));
      resend = timing::Deadline::after(clock_, Console::wake_resend_interval);
    }
  }
}

}  // namespace libchiaking::consoles::usecases
