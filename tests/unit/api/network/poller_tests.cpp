#include "tests/unit/api/network/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace network_test {
namespace {

using InMemoryPoller = InMemoryNetworkTest;

TEST_F(InMemoryPoller, TimesOutBlocksAndWakes) {
  auto receiver = udp(network, loopback());
  auto sender = udp(network, loopback());
  auto poller = network.pollers().create_poller();
  ASSERT_TRUE(poller);
  const std::array<platform::PollRequest, 1> requests{
      {{receiver.get(), 3, platform::PollEvent::readable}}};

  const auto started = monotonic.now_microseconds();
  const auto idle = poller.value()->wait(requests, 40ms, platform::StopToken{});
  ASSERT_TRUE(idle);
  EXPECT_TRUE(idle.value().ready.empty());
  EXPECT_FALSE(idle.value().woke);
  EXPECT_GE(std::chrono::microseconds{static_cast<std::int64_t>(
                monotonic.now_microseconds() - started)},
            40ms);

  auto delayed = after_a_moment([&] {
    EXPECT_TRUE(
        sender->send_to(bytes("go"), receiver->local_endpoint().value()));
  });
  const auto delivered =
      poller.value()->wait(requests, 5s, platform::StopToken{});
  EXPECT_TRUE(delayed.join());
  ASSERT_TRUE(delivered);
  ASSERT_EQ(delivered.value().ready.size(), 1U);
  EXPECT_EQ(delivered.value().ready.front().tag, 3U);
  EXPECT_EQ(
      delivered.value().ready.front().ready & platform::PollEvent::readable,
      platform::PollEvent::readable);

  auto waker = after_a_moment([&] { EXPECT_TRUE(poller.value()->wake()); });
  std::array<std::byte, 8> buffer{};
  EXPECT_TRUE(receiver->receive_from(buffer));
  const auto woken = poller.value()->wait(requests, 5s, platform::StopToken{});
  EXPECT_TRUE(waker.join());
  ASSERT_TRUE(woken);
  EXPECT_TRUE(woken.value().woke);
  EXPECT_TRUE(woken.value().ready.empty());

  EXPECT_TRUE(poller.value()->wake());
  const auto early = poller.value()->wait(requests, 5s, platform::StopToken{});
  ASSERT_TRUE(early);
  EXPECT_TRUE(early.value().woke);
  const auto cleared =
      poller.value()->wait(requests, 10ms, platform::StopToken{});
  ASSERT_TRUE(cleared);
  EXPECT_FALSE(cleared.value().woke);

  fakes::InMemoryNetwork other{threading, monotonic};
  auto foreign = udp(other, loopback());
  const std::array<platform::PollRequest, 1> wrong{
      {{foreign.get(), 1, platform::PollEvent::readable}}};
  const auto rejected =
      poller.value()->wait(wrong, 10ms, platform::StopToken{});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), platform::NetError::wrong_provider);
}

}  // namespace
}  // namespace network_test
