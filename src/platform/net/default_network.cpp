#include <libchiaking/platform/net/default.hpp>

#include "src/platform/net/network_stack.hpp"
#include "src/platform/runtime/pending_call.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace libchiaking::platform {

DefaultSocketProvider::~DefaultSocketProvider() = default;

auto DefaultSocketProvider::create_udp_socket(const IpAddress& family_of)
    -> Expected<std::unique_ptr<UdpSocket>, NetError> {
  return detail::NetworkStack::create_udp_socket(*network_, family_of);
}

auto DefaultSocketProvider::create_tcp_socket(const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpSocket>, NetError> {
  return detail::NetworkStack::create_tcp_socket(*network_, family_of);
}

auto DefaultSocketProvider::create_tcp_listener(const IpAddress& family_of)
    -> Expected<std::unique_ptr<TcpListener>, NetError> {
  return detail::NetworkStack::create_tcp_listener(*network_, family_of);
}

auto DefaultPollerProvider::create_poller()
    -> Expected<std::unique_ptr<Poller>, NetError> {
  return detail::NetworkStack::create_poller(*network_, *clock_);
}

DefaultDnsProvider::DefaultDnsProvider(ThreadingProvider& threading,
                                       const ClockProvider& clock)
    : threading_(&threading), clock_(&clock), abandoned_guard_(threading) {}

DefaultDnsProvider::~DefaultDnsProvider() = default;

void DefaultDnsProvider::keep(Thread lookup) {
  const Lock lock{abandoned_guard_};
  std::erase_if(abandoned_, [](Thread& kept) {
    const auto joined = kept.join_for(std::chrono::nanoseconds::zero());
    return !joined || joined.value();
  });
  abandoned_.push_back(std::move(lookup));
}

auto DefaultDnsProvider::resolve(std::string_view host,
                                 std::string_view service)
    -> Expected<std::vector<IpAddress>, NetError> {
  return detail::NetworkStack::resolve(host, service);
}

auto DefaultDnsProvider::resolve(std::string_view host,
                                 std::string_view service,
                                 const StopToken& stop)
    -> Expected<std::vector<IpAddress>, NetError> {
  using Lookup =
      runtime::PendingCall<Expected<std::vector<IpAddress>, NetError>>;
  const auto refusal = [&stop](runtime::CallUnfinished reason) {
    if (reason == runtime::CallUnfinished::broken)
      return NetError::system_failure;
    if (reason == runtime::CallUnfinished::expired || stop.deadline_expired())
      return NetError::timed_out;
    return NetError::interrupted;
  };
  if (stop.stop_requested())
    return unexpected(refusal(runtime::CallUnfinished::stopped));
  if (auto literal = parse_ip_address(host))
    return std::vector{literal.value()};
  try {
    auto lookup = Lookup::on_own_thread(
        *threading_, *clock_,
        [name = std::string{host}, port = std::string{service}]()
            -> Expected<std::vector<IpAddress>, NetError> {
          try {
            return detail::NetworkStack::resolve(name, port);
          } catch (...) {
            return unexpected(NetError::system_failure);
          }
        });
    if (!lookup) return unexpected(NetError::system_failure);
    auto finished = lookup.value().await(stop, cancellable_resolve_timeout);
    if (!finished) {
      keep(lookup.value().abandon());
      return unexpected(refusal(finished.error()));
    }
    return std::move(finished).value();
  } catch (...) {
    return unexpected(NetError::system_failure);
  }
}

auto DefaultInterfacesProvider::enumerate()
    -> Expected<std::vector<InterfaceInfo>, NetError> {
  return detail::NetworkStack::enumerate();
}

SocketProvider& DefaultNetworkProvider::sockets() noexcept { return sockets_; }

PollerProvider& DefaultNetworkProvider::pollers() noexcept { return pollers_; }

DnsProvider& DefaultNetworkProvider::dns() noexcept { return dns_; }

InterfacesProvider& DefaultNetworkProvider::interfaces() noexcept {
  return interfaces_;
}

NetworkCapabilities DefaultNetworkProvider::capabilities() const noexcept {
  return detail::NetworkStack::capabilities();
}

}  // namespace libchiaking::platform
