/// @file
/// @brief Declares a header-only conformance suite that a host runs against its
/// NetworkProvider to check that it behaves as the library expects.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/net/types.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::platform::net_conformance {

/// @brief Identifies one group of conformance checks; the values are bits of
/// Report::failed_mask.
enum class Check : std::uint32_t {
  /// @brief parse_ip_address and to_string agree with each other and reject
  /// malformed text.
  address_roundtrip = 1u << 0,
  /// @brief A datagram sent between two loopback UDP sockets arrives intact
  /// with the right source endpoint.
  udp_loopback = 1u << 1,
  /// @brief A loopback TCP connection can be established, carries data both
  /// ways and reports the peer's close as NetError::closed.
  tcp_loopback = 1u << 2,
  /// @brief Receiving on an idle UDP socket returns NetError::would_block
  /// instead of waiting.
  would_block = 1u << 3,
  /// @brief Poller::wait with nothing ready returns an empty result
  /// after the timeout and not before.
  poll_timeout = 1u << 4,
  /// @brief Poller::wake from another thread ends a wait, and repeated
  /// wakes are reported once.
  poll_wake = 1u << 5,
  /// @brief A stopped token ends Poller::wait and is reported as
  /// PollWaitResult::stop_requested.
  poll_stop = 1u << 6,
  /// @brief A poller refuses a null pollable and one from another
  /// NetworkProvider with NetError::wrong_provider.
  provider_mixing = 1u << 7,
  /// @brief DnsProvider::resolve handles a numeric address and "localhost",
  /// and fails for a name that does not exist.
  dns_literal_and_localhost = 1u << 8,
  /// @brief InterfacesProvider::enumerate returns named, indexed entries that
  /// include loopback and have consistent broadcast fields.
  interface_invariants = 1u << 9,
  /// @brief NetworkCapabilities, the PlatformFactory capability bits and the
  /// socket options that actually succeed all agree.
  capability_option_consistency = 1u << 10,
  /// @brief The first close() of a socket succeeds, and a second close and
  /// every later call return NetError::closed.
  idempotent_close = 1u << 11,
};

/// @brief Result of a conformance run.
struct Report {
  /// @brief True when every check passed.
  bool ok{true};
  /// @brief Check bits of the group that failed, or 0. The run stops at the
  /// first failure, so at most one bit is set.
  std::uint32_t failed_mask{0};
};

namespace detail {

/// @brief Longest time a check waits for a non-blocking call to finish.
inline constexpr auto deadline = std::chrono::seconds{2};
/// @brief Pause between retries of a non-blocking call.
inline constexpr auto tick = std::chrono::milliseconds{1};

/// @brief Measures time on the platform's monotonic clock.
///
/// @param factory The platform whose clock is read.
/// @return The time since the clock's origin.
inline auto reading(PlatformFactory& factory) noexcept
    -> std::chrono::microseconds {
  return std::chrono::microseconds{
      static_cast<std::int64_t>(factory.clock().now_microseconds())};
}

/// @brief Retries a predicate every tick until it returns true or the deadline
/// passes, pausing with a Sleeper from the platform's ThreadingProvider.
///
/// @tparam Predicate Callable returning something convertible to bool.
/// @param factory The platform whose threading provider and clock pace the
///     retries.
/// @param predicate The condition to wait for.
/// @return True when the predicate succeeded in time.
template <class Predicate>
inline bool spin_until(PlatformFactory& factory, Predicate&& predicate) {
  Sleeper sleeper{factory.threading(), factory.clock()};
  const auto start = reading(factory);
  while (!predicate()) {
    if (reading(factory) - start > deadline) return false;
    static_cast<void>(sleeper.sleep_for(tick));
  }
  return true;
}

/// @brief Marks a check group as failed in the report.
inline void fail(Report& report, Check check) noexcept {
  report.ok = false;
  report.failed_mask |= static_cast<std::uint32_t>(check);
}

/// @brief Runs the Check::address_roundtrip checks.
inline bool check_address_roundtrip(Report& report) {
  const std::array<std::pair<std::string, IpAddress>, 6> cases{{
      {"127.0.0.1", IpAddress::loopback_ipv4()},
      {"0.0.0.0", IpAddress::any_ipv4()},
      {"192.168.1.200", IpAddress::ipv4({192, 168, 1, 200})},
      {"::1", IpAddress::loopback_ipv6()},
      {"::", IpAddress::any_ipv6()},
      {"2001:db8::1", IpAddress::ipv6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 1})},
  }};
  for (const auto& [text, expected] : cases) {
    const auto parsed = parse_ip_address(text);
    if (!parsed || parsed.value() != expected ||
        to_string(parsed.value()) != text) {
      fail(report, Check::address_roundtrip);
      return false;
    }
  }
  if (parse_ip_address("999.1.1.1") || parse_ip_address("1.2.3") ||
      parse_ip_address("01.2.3.4") || parse_ip_address("gg::1") ||
      parse_ip_address("")) {
    fail(report, Check::address_roundtrip);
    return false;
  }
  const auto mapped =
      IpAddress::ipv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1});
  if (!mapped.is_ipv4_mapped() ||
      mapped.unmapped_ipv4() != IpAddress::loopback_ipv4() ||
      !IpAddress::loopback_ipv6().is_loopback() ||
      !IpAddress::any_ipv4().is_unspecified()) {
    fail(report, Check::address_roundtrip);
    return false;
  }
  const SocketEndpoint left{IpAddress::loopback_ipv4(), 9355};
  const SocketEndpoint right{IpAddress::loopback_ipv4(), 9356};
  if (left == right || left.address != right.address) {
    fail(report, Check::address_roundtrip);
    return false;
  }
  return true;
}

/// @brief Runs the Check::udp_loopback checks.
inline bool check_udp_loopback(PlatformFactory& factory,
                               NetworkProvider& network, Report& report) {
  auto first = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto second = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  if (!first || !second) {
    fail(report, Check::udp_loopback);
    return false;
  }
  auto& sender = first.value();
  auto& receiver = second.value();
  if (!receiver->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0}) ||
      !sender->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::udp_loopback);
    return false;
  }
  const auto receiver_endpoint = receiver->local_endpoint();
  if (!receiver_endpoint) {
    fail(report, Check::udp_loopback);
    return false;
  }
  const std::array<std::byte, 5> payload{
      std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11}};
  const auto sent = sender->send_to(payload, receiver_endpoint.value());
  if (!sent || sent.value() != payload.size()) {
    fail(report, Check::udp_loopback);
    return false;
  }
  std::array<std::byte, 64> buffer{};
  auto received = receiver->receive_from(buffer);
  if (!received && received.error() == NetError::would_block) {
    const bool completed = spin_until(factory, [&] {
      received = receiver->receive_from(buffer);
      return received || received.error() != NetError::would_block;
    });
    if (!completed) {
      fail(report, Check::udp_loopback);
      return false;
    }
  }
  if (!received || received.value().size != payload.size() ||
      std::memcmp(buffer.data(), payload.data(), payload.size()) != 0 ||
      received.value().source.address != IpAddress::loopback_ipv4() ||
      received.value().source.port != sender->local_endpoint().value().port) {
    fail(report, Check::udp_loopback);
    return false;
  }
  return true;
}

/// @brief Runs the Check::tcp_loopback checks.
inline bool check_tcp_loopback(PlatformFactory& factory,
                               NetworkProvider& network, Report& report) {
  auto listener_result =
      network.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
  auto client_result =
      network.sockets().create_tcp_socket(IpAddress::loopback_ipv4());
  if (!listener_result || !client_result) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  auto& listener = listener_result.value();
  auto& client = client_result.value();
  if (!listener->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0}) ||
      !listener->listen(4)) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  const auto listener_endpoint = listener->local_endpoint();
  if (!listener_endpoint) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  if (listener->accept()) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  const auto connected = client->connect(listener_endpoint.value());
  if (!connected && connected.error() != NetError::would_block) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  if (!connected) {
    auto poller = network.pollers().create_poller();
    if (!poller) {
      fail(report, Check::tcp_loopback);
      return false;
    }
    const std::array<PollRequest, 1> requests{
        {{client.get(), 1, PollEvent::writable}}};
    const auto ready = poller.value()->wait(requests, deadline, StopToken{});
    if (!ready || ready.value().ready.empty() || !client->complete_connect()) {
      fail(report, Check::tcp_loopback);
      return false;
    }
  }
  auto accepted = listener->accept();
  if (!accepted && accepted.error() == NetError::would_block) {
    const bool completed = spin_until(factory, [&] {
      accepted = listener->accept();
      return accepted || accepted.error() != NetError::would_block;
    });
    if (!completed) {
      fail(report, Check::tcp_loopback);
      return false;
    }
  }
  if (!accepted) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  const auto client_peer = client->peer_endpoint();
  if (!client_peer || client_peer.value() != listener_endpoint.value()) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  const std::array<std::byte, 4> payload{std::byte{1}, std::byte{2},
                                         std::byte{3}, std::byte{4}};
  auto sent = client->send(payload);
  if (!sent || sent.value() != payload.size()) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  const auto receive_payload = [&factory](TcpSocket& socket,
                                          std::span<const std::byte> expected,
                                          std::span<std::byte> buffer) -> bool {
    std::size_t offset = 0;
    bool hard_error = false;
    const bool completed = spin_until(factory, [&] {
      const auto result =
          socket.receive(buffer.subspan(offset, expected.size() - offset));
      if (result) {
        if (result.value() == 0 || result.value() > expected.size() - offset) {
          hard_error = true;
          return true;
        }
        offset += result.value();
        return offset == expected.size();
      }
      if (result.error() == NetError::would_block) return false;
      hard_error = true;
      return true;
    });
    return completed && !hard_error && offset == expected.size() &&
           std::memcmp(buffer.data(), expected.data(), expected.size()) == 0;
  };
  std::array<std::byte, 64> buffer{};
  if (!receive_payload(*accepted.value(), payload, buffer)) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  sent = accepted.value()->send(payload);
  if (!sent) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  if (!receive_payload(*client, payload, buffer)) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  (void)client->close();
  auto received = accepted.value()->receive(buffer);
  if (!received && received.error() == NetError::would_block) {
    const bool completed = spin_until(factory, [&] {
      received = accepted.value()->receive(buffer);
      return received || received.error() != NetError::would_block;
    });
    if (!completed) {
      fail(report, Check::tcp_loopback);
      return false;
    }
  }
  if (received || received.error() != NetError::closed) {
    fail(report, Check::tcp_loopback);
    return false;
  }
  return true;
}

/// @brief Runs the Check::would_block checks.
inline bool check_would_block(NetworkProvider& network, Report& report) {
  auto socket = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  if (!socket ||
      !socket.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::would_block);
    return false;
  }
  std::array<std::byte, 16> buffer{};
  const auto received = socket.value()->receive_from(buffer);
  if (received || received.error() != NetError::would_block) {
    fail(report, Check::would_block);
    return false;
  }
  return true;
}

/// @brief Runs the Check::poll_timeout checks.
inline bool check_poll_timeout(PlatformFactory& factory,
                               NetworkProvider& network, Report& report) {
  auto socket = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  if (!socket || !poller) {
    fail(report, Check::poll_timeout);
    return false;
  }
  if (!socket.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::poll_timeout);
    return false;
  }
  StopToken token;
  const std::array<PollRequest, 1> requests{
      {{socket.value().get(), 42, PollEvent::readable}}};
  const auto start = reading(factory);
  const auto result =
      poller.value()->wait(requests, std::chrono::milliseconds{80}, token);
  const auto elapsed = reading(factory) - start;
  if (!result || result.value().woke || result.value().stop_requested ||
      !result.value().ready.empty() ||
      elapsed < std::chrono::milliseconds{60}) {
    fail(report, Check::poll_timeout);
    return false;
  }
  return true;
}

/// @brief Runs the Check::poll_wake checks, using a thread from the factory's
/// ThreadingProvider to call wake.
inline bool check_poll_wake(PlatformFactory& factory, NetworkProvider& network,
                            Report& report) {
  auto socket = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  auto stop_source = factory.threading().create_stop_source();
  if (!socket || !poller || !stop_source) {
    fail(report, Check::poll_wake);
    return false;
  }
  if (!socket.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::poll_wake);
    return false;
  }
  struct Waker {
    Poller* poller;
    Sleeper* sleeper;
  };
  Sleeper sleeper{factory.threading(), factory.clock()};
  Waker waking{poller.value().get(), &sleeper};
  auto waker = factory.threading().start_thread(
      {[](void* context) noexcept {
         auto* target = static_cast<Waker*>(context);
         static_cast<void>(
             target->sleeper->sleep_for(std::chrono::milliseconds{30}));
         (void)target->poller->wake();
       },
       &waking});
  if (!waker) {
    fail(report, Check::poll_wake);
    return false;
  }
  StopToken token = stop_source.value()->token();
  const std::array<PollRequest, 1> requests{
      {{socket.value().get(), 7, PollEvent::readable}}};
  const auto start = reading(factory);
  const auto result =
      poller.value()->wait(requests, std::chrono::seconds{2}, token);
  const auto elapsed = reading(factory) - start;
  const bool joined = waker.value()->join() && !waker.value()->joinable();
  if (!result || !result.value().woke || !result.value().ready.empty() ||
      result.value().stop_requested || elapsed > std::chrono::seconds{1} ||
      !joined) {
    fail(report, Check::poll_wake);
    return false;
  }
  for (int round = 0; round < 3; ++round) {
    const int repeats = round == 1 ? 20000 : 4;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      if (!poller.value()->wake()) {
        fail(report, Check::poll_wake);
        return false;
      }
    }
    const auto coalesced =
        poller.value()->wait(requests, std::chrono::seconds{1}, token);
    if (!coalesced || !coalesced.value().woke ||
        !coalesced.value().ready.empty()) {
      fail(report, Check::poll_wake);
      return false;
    }
    const auto drained =
        poller.value()->wait(requests, std::chrono::milliseconds{30}, token);
    if (!drained || drained.value().woke || !drained.value().ready.empty()) {
      fail(report, Check::poll_wake);
      return false;
    }
  }
  return true;
}

/// @brief Stop state the suite can trigger by hand, without a ThreadingProvider
/// stop source.
class DetachedStopState final : public StopState {
 public:
  /// @brief Marks the state as stopped; safe from any thread.
  void request() noexcept { requested_.store(true, std::memory_order_release); }
  /// @brief Reports whether request() has been called.
  bool stop_requested() const noexcept override {
    return requested_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> requested_{false};
};

/// @brief Checks that a wait given an already stopped token returns at once
/// with PollWaitResult::stop_requested.
inline bool check_poll_stop(PlatformFactory& factory, NetworkProvider& network,
                            Report& report) {
  auto socket = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  if (!socket || !poller) {
    fail(report, Check::poll_stop);
    return false;
  }
  auto state = std::make_shared<DetachedStopState>();
  state->request();
  StopToken token(state);
  const std::array<PollRequest, 1> requests{
      {{socket.value().get(), 1, PollEvent::readable}}};
  const auto start = reading(factory);
  const auto result =
      poller.value()->wait(requests, std::chrono::seconds{2}, token);
  if (!result || !result.value().stop_requested || result.value().woke ||
      reading(factory) - start > std::chrono::seconds{1}) {
    fail(report, Check::poll_stop);
    return false;
  }
  return true;
}

/// @brief Checks that stopping the token and then calling wake ends a wait in
/// progress and is reported as a stop, not a wake.
inline bool check_poll_stop_signaled(PlatformFactory& factory,
                                     NetworkProvider& network, Report& report) {
  auto socket = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  if (!socket || !poller) {
    fail(report, Check::poll_stop);
    return false;
  }
  if (!socket.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::poll_stop);
    return false;
  }
  auto state = std::make_shared<DetachedStopState>();
  StopToken token(state);
  Poller* poller_pointer = poller.value().get();
  auto stopper = Thread::start(
      factory.threading(), "net conformance stopper", ThreadRole::unspecified,
      [&factory, poller_pointer, state] {
        Sleeper sleeper{factory.threading(), factory.clock()};
        static_cast<void>(sleeper.sleep_for(std::chrono::milliseconds{30}));
        state->request();
        (void)poller_pointer->wake();
      });
  if (!stopper) {
    fail(report, Check::poll_stop);
    return false;
  }
  const std::array<PollRequest, 1> requests{
      {{socket.value().get(), 1, PollEvent::readable}}};
  const auto result =
      poller.value()->wait(requests, std::chrono::seconds{2}, token);
  const auto joined = stopper.value().join();
  if (!joined || !result || !result.value().stop_requested ||
      result.value().woke) {
    fail(report, Check::poll_stop);
    return false;
  }
  return true;
}

/// @brief Runs the Check::provider_mixing checks.
inline bool check_provider_mixing(NetworkProvider& owned,
                                  NetworkProvider& foreign, Report& report) {
  auto socket = owned.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = foreign.pollers().create_poller();
  if (!socket || !poller) {
    fail(report, Check::provider_mixing);
    return false;
  }
  if (!socket.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0})) {
    fail(report, Check::provider_mixing);
    return false;
  }
  StopToken token;
  const std::array<PollRequest, 1> requests{
      {{socket.value().get(), 1, PollEvent::readable}}};
  const auto result =
      poller.value()->wait(requests, std::chrono::milliseconds{50}, token);
  if (result || result.error() != NetError::wrong_provider) {
    fail(report, Check::provider_mixing);
    return false;
  }
  const std::array<PollRequest, 1> null_requests{
      {{nullptr, 1, PollEvent::readable}}};
  const auto null_result =
      poller.value()->wait(null_requests, std::chrono::milliseconds{0}, token);
  if (null_result || null_result.error() != NetError::wrong_provider) {
    fail(report, Check::provider_mixing);
    return false;
  }
  return true;
}

/// @brief Runs the Check::dns_literal_and_localhost checks.
inline bool check_dns(NetworkProvider& network, Report& report) {
  const auto literal = network.dns().resolve("127.0.0.1", "");
  if (!literal) {
    fail(report, Check::dns_literal_and_localhost);
    return false;
  }
  bool found = false;
  for (const auto& address : literal.value()) {
    if (address == IpAddress::loopback_ipv4()) found = true;
  }
  const auto localhost = network.dns().resolve("localhost", "");
  if (!found || !localhost || localhost.value().empty()) {
    fail(report, Check::dns_literal_and_localhost);
    return false;
  }
  if (network.dns().resolve("this-host-does-not-exist-libchiaki-invalid", "")) {
    fail(report, Check::dns_literal_and_localhost);
    return false;
  }
  return true;
}

/// @brief Runs the Check::interface_invariants checks.
inline bool check_interfaces(NetworkProvider& network, Report& report) {
  const auto interfaces = network.interfaces().enumerate();
  if (!interfaces || interfaces.value().empty()) {
    fail(report, Check::interface_invariants);
    return false;
  }
  bool saw_loopback = false;
  for (const auto& entry : interfaces.value()) {
    if (entry.name.empty() || entry.index == 0) {
      fail(report, Check::interface_invariants);
      return false;
    }
    if (entry.loopback) saw_loopback = true;
    if (entry.has_broadcast &&
        (!entry.address.is_ipv4() || !entry.broadcast.is_ipv4() ||
         entry.broadcast == IpAddress::any_ipv4())) {
      fail(report, Check::interface_invariants);
      return false;
    }
  }
  if (!saw_loopback) {
    fail(report, Check::interface_invariants);
    return false;
  }
  return true;
}

/// @brief Runs the Check::capability_option_consistency checks.
inline bool check_capability_option_consistency(PlatformFactory& factory,
                                                NetworkProvider& network,
                                                Report& report) {
  const auto info = factory.info();
  const auto capabilities = network.capabilities();
  if ((info.capabilities & PlatformCapability::network) !=
      PlatformCapability::network) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  const bool advertised_v4 =
      (info.capabilities & PlatformCapability::socket_dontfrag_v4) ==
      PlatformCapability::socket_dontfrag_v4;
  const bool advertised_v6 =
      (info.capabilities & PlatformCapability::socket_dontfrag_v6) ==
      PlatformCapability::socket_dontfrag_v6;
  if (advertised_v4 != capabilities.socket_dont_fragment_ipv4 ||
      advertised_v6 != capabilities.socket_dont_fragment_ipv6) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  auto udp = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto tcp = network.sockets().create_tcp_socket(IpAddress::loopback_ipv4());
  if (!udp || !tcp) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  if ((capabilities.broadcast &&
       (!udp.value()->set_boolean_option(SocketOption::broadcast, true) ||
        !udp.value()->set_boolean_option(SocketOption::broadcast, false))) ||
      !udp.value()->set_boolean_option(SocketOption::reuse_address, true) ||
      !udp.value()->set_receive_buffer_size(64 * 1024)) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  const auto v4_dontfrag =
      udp.value()->set_boolean_option(SocketOption::dont_fragment, true);
  if (v4_dontfrag.has_value() != capabilities.socket_dont_fragment_ipv4) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  auto udp6 = network.sockets().create_udp_socket(IpAddress::loopback_ipv6());
  if (udp6) {
    const auto v6_dontfrag =
        udp6.value()->set_boolean_option(SocketOption::dont_fragment, true);
    if (v6_dontfrag.has_value() != capabilities.socket_dont_fragment_ipv6) {
      fail(report, Check::capability_option_consistency);
      return false;
    }
  } else if (capabilities.socket_dont_fragment_ipv6 ||
             (udp6.error() != NetError::not_supported &&
              udp6.error() != NetError::address_not_available &&
              udp6.error() != NetError::network_unreachable)) {
    fail(report, Check::capability_option_consistency);
    return false;
  }
  return true;
}

/// @brief Runs the Check::idempotent_close checks.
inline bool check_idempotent_close(NetworkProvider& network, Report& report) {
  auto udp = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto tcp = network.sockets().create_tcp_socket(IpAddress::loopback_ipv4());
  auto listener =
      network.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
  if (!udp || !tcp || !listener) {
    fail(report, Check::idempotent_close);
    return false;
  }
  const auto udp_close = udp.value()->close();
  const auto udp_close_again = udp.value()->close();
  if (!udp_close || udp_close_again ||
      udp_close_again.error() != NetError::closed) {
    fail(report, Check::idempotent_close);
    return false;
  }
  const auto tcp_close = tcp.value()->close();
  const auto tcp_close_again = tcp.value()->close();
  if (!tcp_close || tcp_close_again ||
      tcp_close_again.error() != NetError::closed) {
    fail(report, Check::idempotent_close);
    return false;
  }
  const auto listener_close = listener.value()->close();
  const auto listener_close_again = listener.value()->close();
  if (!listener_close || listener_close_again ||
      listener_close_again.error() != NetError::closed) {
    fail(report, Check::idempotent_close);
    return false;
  }
  std::array<std::byte, 8> buffer{};
  const auto udp_receive = udp.value()->receive_from(buffer);
  const auto tcp_receive = tcp.value()->receive(buffer);
  const auto listener_accept = listener.value()->accept();
  const auto udp_endpoint = udp.value()->local_endpoint();
  if (udp_receive || udp_receive.error() != NetError::closed || tcp_receive ||
      tcp_receive.error() != NetError::closed || listener_accept ||
      listener_accept.error() != NetError::closed || udp_endpoint ||
      udp_endpoint.error() != NetError::closed) {
    fail(report, Check::idempotent_close);
    return false;
  }
  return true;
}

}  // namespace detail

/// @brief Runs the whole networking conformance suite against a platform.
///
/// A host that supplies its own NetworkProvider calls this from a test to find
/// out whether the library can rely on it. The checks open real sockets on the
/// loopback interface, start a thread and wait on timeouts, so a run can take
/// several seconds. It stops at the first group that fails.
///
/// @param factory The platform under test. Its PlatformFactory::network is
///     exercised, its ThreadingProvider supplies threads, pauses and a stop
///     source, and its monotonic clock measures the waits.
/// @param foreign A second platform with a different NetworkProvider instance,
///     used to check that sockets and pollers from different providers are not
///     mixed.
/// @return Report::ok true when every check passed; otherwise
///     Report::failed_mask holds the Check bit of the first failing group.
inline Report run(PlatformFactory& factory, PlatformFactory& foreign) {
  Report report;
  NetworkProvider& network = factory.network();
  NetworkProvider& foreign_network = foreign.network();
  if (!detail::check_address_roundtrip(report)) return report;
  if (!detail::check_capability_option_consistency(factory, network, report))
    return report;
  if (!detail::check_udp_loopback(factory, network, report)) return report;
  if (!detail::check_would_block(network, report)) return report;
  if (!detail::check_tcp_loopback(factory, network, report)) return report;
  if (!detail::check_idempotent_close(network, report)) return report;
  if (!detail::check_poll_timeout(factory, network, report)) return report;
  if (!detail::check_poll_stop(factory, network, report)) return report;
  if (!detail::check_poll_stop_signaled(factory, network, report))
    return report;
  if (!detail::check_poll_wake(factory, network, report)) return report;
  if (!detail::check_provider_mixing(network, foreign_network, report))
    return report;
  if (!detail::check_dns(network, report)) return report;
  if (!detail::check_interfaces(network, report)) return report;
  return report;
}

}  // namespace libchiaking::platform::net_conformance
