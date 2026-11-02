#include "libchiaking/platform/default_factory.hpp"
#include "src/core/play/adapters/links/active_stream.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace play_test {

using test_support::LiveProviders;
namespace {

namespace platform = libchiaking::platform;
using Waiter = libchiaking::play::ActiveStream::RouteWaiter;
using Pacing = Waiter::Pacing;
using std::chrono::milliseconds;
using std::chrono::seconds;

class RouteWaiter : public ::testing::Test {
 protected:
  static auto now() -> std::chrono::microseconds {
    return std::chrono::microseconds{static_cast<std::int64_t>(LiveProviders::steady_us())};
  }

  auto bound() -> std::unique_ptr<platform::UdpSocket> {
    auto socket = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(socket);
    if (!socket) return nullptr;
    EXPECT_TRUE(
        socket.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    return std::move(socket.value());
  }

  auto unbound() -> std::unique_ptr<platform::UdpSocket> {
    auto socket = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(socket);
    if (!socket) return nullptr;
    return std::move(socket.value());
  }

  void send_to(platform::UdpSocket& sender, platform::UdpSocket& receiver) {
    const auto endpoint = receiver.local_endpoint();
    ASSERT_TRUE(endpoint);
    const std::array<std::byte, 4> payload{};
    EXPECT_TRUE(sender.send_to(payload, endpoint.value()));
    LiveProviders::nap(milliseconds{20});
  }

  auto network() -> platform::NetworkProvider& { return network_; }
  auto stop() -> test_support::AtomicStop& { return *stop_; }
  auto waiter() -> Waiter& { return waiter_; }

 private:
  platform::DefaultPlatformFactory factory_;
  platform::NetworkProvider& network_ = factory_.network();
  std::shared_ptr<test_support::AtomicStop> stop_ =
      std::make_shared<test_support::AtomicStop>();
  Waiter waiter_{network_, factory_.threading(), factory_.clock(),
                 platform::StopToken{stop_}};
};

TEST_F(RouteWaiter, WaitsOutTheDeadlineWithNothingToWatch) {
  const auto started = now();
  waiter().wait(milliseconds{40}, nullptr);
  const auto elapsed = now() - started;
  EXPECT_GE(elapsed, milliseconds{35});
  EXPECT_LT(elapsed, seconds{2});
}

TEST_F(RouteWaiter, CutsAWaitOnAQuietRouteToTheProtocolTick) {
  const auto receiver = bound();
  ASSERT_NE(receiver, nullptr);
  const auto started = now();
  waiter().wait(seconds{5}, receiver.get());
  const auto elapsed = now() - started;
  EXPECT_GE(elapsed, Waiter::protocol_tick - milliseconds{1});
  EXPECT_LT(elapsed, seconds{1});
}

TEST_F(RouteWaiter, ReturnsBeforeTheTickWhenTheRouteIsReadable) {
  const auto receiver = bound();
  const auto sender = unbound();
  ASSERT_NE(receiver, nullptr);
  ASSERT_NE(sender, nullptr);
  send_to(*sender, *receiver);
  const auto started = now();
  waiter().wait(seconds{5}, receiver.get());
  EXPECT_LT(now() - started, Waiter::protocol_tick);
}

TEST_F(RouteWaiter, WakesWhenWoken) {
  auto waker = LiveProviders::spawn("waker", [this] {
    LiveProviders::nap(milliseconds{50});
    waiter().wake();
  });
  const auto started = now();
  waiter().wait(seconds{5}, nullptr);
  const auto elapsed = now() - started;
  static_cast<void>(waker.join());
  EXPECT_GE(elapsed, milliseconds{40});
  EXPECT_LT(elapsed, seconds{2});
}

TEST_F(RouteWaiter, WakesWhenCancelledAndStaysAwake) {
  auto canceller = LiveProviders::spawn("canceller", [this] {
    LiveProviders::nap(milliseconds{50});
    stop().stopped = true;
  });
  auto started = now();
  waiter().wait(seconds{5}, nullptr);
  const auto elapsed = now() - started;
  static_cast<void>(canceller.join());
  EXPECT_GE(elapsed, milliseconds{40});
  EXPECT_LT(elapsed, seconds{2});

  started = now();
  waiter().wait(seconds{5}, nullptr);
  EXPECT_LT(now() - started, milliseconds{100});
}

TEST_F(RouteWaiter, CutsAnEstablishingWaitToTheProtocolTick) {
  stop().stopped = true;
  waiter().wait(seconds{5}, nullptr);
  stop().stopped = false;
  const auto started = now();
  waiter().wait(seconds{5}, nullptr, true);
  const auto elapsed = now() - started;
  EXPECT_GE(elapsed, Waiter::protocol_tick - milliseconds{1});
  EXPECT_LT(elapsed, seconds{1});
}

TEST_F(RouteWaiter, ReportsWhichWatchedRouteIsReadable) {
  const auto route = bound();
  const auto ctrl = bound();
  const auto sender = unbound();
  ASSERT_NE(route, nullptr);
  ASSERT_NE(ctrl, nullptr);
  ASSERT_NE(sender, nullptr);
  ASSERT_TRUE(route->local_endpoint());
  ASSERT_TRUE(ctrl->local_endpoint());

  EXPECT_EQ(waiter().wait(milliseconds{20}, {route.get(), ctrl.get()}, true),
            0U);

  send_to(*sender, *route);
  EXPECT_EQ(waiter().wait(seconds{1}, {route.get(), ctrl.get()}, true), 1U);
  EXPECT_EQ(waiter().wait(seconds{1}, {nullptr, route.get()}, true), 2U);

  send_to(*sender, *ctrl);
  EXPECT_EQ(waiter().wait(seconds{1}, {route.get(), ctrl.get()}, true), 3U);
  EXPECT_EQ(waiter().wait(seconds{1}, {ctrl.get(), ctrl.get()}, true), 3U);
}

TEST_F(RouteWaiter, WaitsPastTheProtocolTickOnlyWhenScheduled) {
  ASSERT_FALSE(network().capabilities().costly_socket_calls);
  const auto route = bound();
  ASSERT_NE(route, nullptr);

  auto started = LiveProviders::steady_us();
  EXPECT_EQ(waiter().wait(milliseconds{40}, {route.get()}, Pacing::ticked), 0U);
  EXPECT_LT(LiveProviders::steady_us() - started, 30'000U);

  started = LiveProviders::steady_us();
  EXPECT_EQ(waiter().wait(milliseconds{40}, {route.get()}, Pacing::scheduled),
            0U);
  EXPECT_GE(LiveProviders::steady_us() - started, 35'000U);

  EXPECT_EQ(waiter().wait(std::chrono::nanoseconds{0}, {route.get()},
                          Pacing::scheduled),
            0U);
}

TEST_F(RouteWaiter, ChecksTheRouteOnceWhenTheDeadlineHasPassed) {
  const auto route = bound();
  const auto sender = unbound();
  ASSERT_NE(route, nullptr);
  ASSERT_NE(sender, nullptr);
  send_to(*sender, *route);

  const auto started = LiveProviders::steady_us();
  EXPECT_EQ(waiter().wait(std::chrono::nanoseconds{0}, {route.get()},
                          Pacing::scheduled),
            1U);
  EXPECT_LT(LiveProviders::steady_us() - started, 30'000U);
  EXPECT_FALSE(
      waiter().wait(std::chrono::nanoseconds{0}, {nullptr}, Pacing::scheduled));
}

}  // namespace
}  // namespace play_test
