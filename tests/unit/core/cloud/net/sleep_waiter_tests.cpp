#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>

namespace cloud_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::net::http::SleepWaiter;

class CountingStop final : public platform::StopState {
 public:
  explicit CountingStop(unsigned stop_at) : stop_at_(stop_at) {}

  auto stop_requested() const noexcept -> bool override {
    return ++checks > stop_at_;
  }

  mutable std::atomic<unsigned> checks{};

 private:
  unsigned stop_at_;
};

class SleepWaiterTiming : public ::testing::Test {
 protected:
  SleepWaiter waiter{test_support::process_threading(), PingRig::monotonic()};
  const platform::StopToken no_stop;
};

TEST_F(SleepWaiterTiming, ChecksTheStopEveryTenthOfASecond) {
  auto running = std::make_shared<CountingStop>(1000);
  const auto before = PingRig::monotonic().now_microseconds();
  EXPECT_TRUE(waiter.wait_milliseconds(1000, platform::StopToken{running}));
  const auto elapsed = PingRig::elapsed_since(before);
  EXPECT_GE(running->checks, 10U);
  EXPECT_GE(elapsed, std::chrono::milliseconds{1000});
}

TEST_F(SleepWaiterTiming, EndsTheStepAfterAStop) {
  auto stopping = std::make_shared<CountingStop>(2);
  const auto stop_started = PingRig::monotonic().now_microseconds();
  EXPECT_FALSE(waiter.wait_milliseconds(2000, platform::StopToken{stopping}));
  const auto stop_elapsed = PingRig::elapsed_since(stop_started);
  EXPECT_EQ(stopping->checks, 3U);
  EXPECT_LT(stop_elapsed, std::chrono::milliseconds{1000});
}

TEST_F(SleepWaiterTiming, RefusesANegativeWait) {
  EXPECT_TRUE(waiter.wait_milliseconds(1, no_stop));
  EXPECT_TRUE(waiter.wait_milliseconds(0, no_stop));
  EXPECT_FALSE(waiter.wait_milliseconds(-1, no_stop));
}

TEST_F(SleepWaiterTiming, RefusesToWaitOnceStoppedThroughEitherInterface) {
  auto stopped_state = std::make_shared<test_support::StopState>();
  stopped_state->stopped = true;
  const platform::StopToken stopped{stopped_state};
  libchiaking::net::http::Waiter& timed_waiter = waiter;
  libchiaking::net::http::PollWaiter& ping_waiter = waiter;
  EXPECT_FALSE(timed_waiter.wait_milliseconds(1000, stopped));
  EXPECT_FALSE(ping_waiter.wait_milliseconds(1, stopped));
}

class SleepWaiterSockets : public ::testing::Test {
 protected:
  void SetUp() override {
    auto made_receiver = network.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    auto made_sender = network.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    ASSERT_TRUE(made_receiver);
    ASSERT_TRUE(made_sender);
    receiver = std::move(made_receiver.value());
    sender = std::move(made_sender.value());
    ASSERT_TRUE(receiver->bind({platform::IpAddress::loopback_ipv4(), 0}));
    ASSERT_TRUE(sender->bind({platform::IpAddress::loopback_ipv4(), 0}));
    const auto bound = receiver->local_endpoint();
    ASSERT_TRUE(bound);
    endpoint = bound.value();
    sources[0] = receiver.get();
  }

  void TearDown() override {
    if (receiver) {
      EXPECT_TRUE(receiver->close());
    }
    if (sender) {
      EXPECT_TRUE(sender->close());
    }
  }

  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           PingRig::monotonic()};
  SleepWaiter waiter{test_support::process_threading(), PingRig::monotonic(),
                     network};
  std::unique_ptr<platform::UdpSocket> receiver;
  std::unique_ptr<platform::UdpSocket> sender;
  platform::SocketEndpoint endpoint{};
  const platform::Pollable* sources[1]{};
  const platform::StopToken running;
};

TEST_F(SleepWaiterSockets, WaitsTheWholeTimeoutWhenNothingArrives) {
  const auto started = PingRig::monotonic().now_microseconds();
  EXPECT_TRUE(waiter.wait_readable(sources, 20, running));
  EXPECT_GE(PingRig::elapsed_since(started), std::chrono::milliseconds{15});
}

TEST_F(SleepWaiterSockets, WakesWhenTheSocketBecomesReadable) {
  auto peer = platform::Thread::start(
                  test_support::process_threading(), "peer",
                  platform::ThreadRole::unspecified,
                  [&] {
                    PingRig::pause_for(std::chrono::milliseconds{30});
                    const std::array<std::byte, 4> bytes{};
                    const auto sent = sender->send_to(bytes, endpoint);
                    EXPECT_TRUE(sent);
                    if (sent) {
                      EXPECT_EQ(sent.value(), bytes.size());
                    }
                  })
                  .value();
  const auto started = PingRig::monotonic().now_microseconds();
  EXPECT_TRUE(waiter.wait_readable(sources, 5'000, running));
  const auto waited = PingRig::elapsed_since(started);
  EXPECT_TRUE(peer.join());
  EXPECT_GE(waited, std::chrono::milliseconds{20});
  EXPECT_LT(waited, std::chrono::milliseconds{1'000});
  std::array<std::byte, 16> buffer{};
  const auto received = receiver->receive_from(buffer);
  ASSERT_TRUE(received);
  EXPECT_EQ(received.value().size, std::size_t{4});
}

TEST_F(SleepWaiterSockets, RefusesToWaitOnceStoppedOrForANegativeTimeout) {
  auto stopped_state = std::make_shared<test_support::StopState>();
  stopped_state->stopped = true;
  EXPECT_FALSE(
      waiter.wait_readable(sources, 5'000, platform::StopToken{stopped_state}));
  EXPECT_FALSE(waiter.wait_readable(sources, -1, running));
}

}  // namespace
}  // namespace cloud_test
