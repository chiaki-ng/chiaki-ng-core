#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

thread_local std::size_t allocations = 0;

using libchiaking::platform::DefaultPlatformFactory;
using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::PollEvent;
using libchiaking::platform::PollRequest;
using libchiaking::platform::SocketEndpoint;
using libchiaking::platform::StopToken;

class PollerReuse : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(poller);
    ASSERT_TRUE(
        first.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0}));
    ASSERT_TRUE(
        second.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0}));
  }

  DefaultPlatformFactory factory;
  decltype(factory.network().sockets().create_udp_socket(
      IpAddress::loopback_ipv4())) first =
      factory.network().sockets().create_udp_socket(IpAddress::loopback_ipv4());
  decltype(first) second =
      factory.network().sockets().create_udp_socket(IpAddress::loopback_ipv4());
  decltype(factory.network().pollers().create_poller()) poller =
      factory.network().pollers().create_poller();
};

TEST_F(PollerReuse, RepeatedWaitsOnTheSameSocketsAllocateNothing) {
  const std::array<PollRequest, 1> one{
      {{first.value().get(), 1, PollEvent::readable}}};
  ASSERT_TRUE(
      poller.value()->wait(one, std::chrono::milliseconds{0}, StopToken{}));
  int failed_waits = 0;
  int ready_waits = 0;
  const auto before = allocations;
  for (int wait = 0; wait < 100; ++wait) {
    const auto waited =
        poller.value()->wait(one, std::chrono::milliseconds{0}, StopToken{});
    if (!waited)
      ++failed_waits;
    else if (!waited.value().ready.empty())
      ++ready_waits;
  }
  const auto after = allocations;
  EXPECT_EQ(failed_waits, 0);
  EXPECT_EQ(ready_waits, 0);
  EXPECT_EQ(after, before);
}

TEST_F(PollerReuse, AReusedPollerStillReportsTheReadySocket) {
  const std::array<PollRequest, 1> one{
      {{first.value().get(), 1, PollEvent::readable}}};
  ASSERT_TRUE(
      poller.value()->wait(one, std::chrono::milliseconds{0}, StopToken{}));
  const auto target = second.value()->local_endpoint();
  ASSERT_TRUE(target);
  const std::array<std::byte, 1> datagram{std::byte{7}};
  ASSERT_TRUE(first.value()->send_to(datagram, target.value()));
  const std::array<PollRequest, 2> both{
      {{first.value().get(), 1, PollEvent::readable},
       {second.value().get(), 2, PollEvent::readable}}};
  const auto ready =
      poller.value()->wait(both, std::chrono::seconds{2}, StopToken{});
  ASSERT_TRUE(ready);
  ASSERT_EQ(ready.value().ready.size(), 1U);
  EXPECT_EQ(ready.value().ready[0].tag, 2U);

  const auto quiet =
      poller.value()->wait(one, std::chrono::milliseconds{0}, StopToken{});
  ASSERT_TRUE(quiet);
  EXPECT_TRUE(quiet.value().ready.empty());

  const std::array<PollRequest, 1> other{
      {{second.value().get(), 3, PollEvent::readable}}};
  const auto still =
      poller.value()->wait(other, std::chrono::milliseconds{0}, StopToken{});
  ASSERT_TRUE(still);
  ASSERT_EQ(still.value().ready.size(), 1U);
  EXPECT_EQ(still.value().ready[0].tag, 3U);

  ASSERT_TRUE(second.value()->close());
  const auto closed =
      poller.value()->wait(other, std::chrono::milliseconds{0}, StopToken{});
  ASSERT_FALSE(closed);
  EXPECT_EQ(closed.error(), NetError::system_failure);
}

}  // namespace

void* operator new(std::size_t size) {
  ++allocations;
  if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
  throw std::bad_alloc{};
}

void operator delete(void* block) noexcept { std::free(block); }

void operator delete(void* block, std::size_t) noexcept { std::free(block); }
