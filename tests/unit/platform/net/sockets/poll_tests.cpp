#include "tests/unit/platform/net/sockets/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace platform_net_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::PollEvent;
using libchiaking::platform::PollRequest;
using libchiaking::platform::StopToken;
using libchiaking::platform::UdpSocket;

class SocketPolling : public SocketRig {
 protected:
  static void raise_descriptor_limit() {
#if !defined(_WIN32)
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < 1024) {
      limit.rlim_cur = limit.rlim_max < 1024 ? limit.rlim_max : 1024;
      static_cast<void>(::setrlimit(RLIMIT_NOFILE, &limit));
    }
#endif
  }

  const StopToken token;
};

TEST_F(SocketPolling, FindsTheOneReadySocketAmongThreeHundred) {
  constexpr std::size_t socket_count = 300;
  raise_descriptor_limit();
  std::vector<std::unique_ptr<UdpSocket>> udp_sockets;
  std::vector<PollRequest> requests;
  udp_sockets.reserve(socket_count);
  requests.reserve(socket_count);
  for (std::size_t index = 0; index < socket_count; ++index) {
    auto created = sockets().create_udp_socket(IpAddress::loopback_ipv4());
    ASSERT_TRUE(created) << "socket " << index;
    auto socket = std::move(created.value());
    ASSERT_TRUE(socket->bind({IpAddress::loopback_ipv4(), 0}))
        << "socket " << index;
    requests.push_back(PollRequest{.pollable = socket.get(),
                                   .tag = index + 1U,
                                   .interest = PollEvent::readable});
    udp_sockets.push_back(std::move(socket));
  }
  const auto target = udp_sockets.back()->local_endpoint();
  ASSERT_TRUE(target);
  const std::array<std::byte, 1> datagram{std::byte{0x5a}};
  const auto sent = udp_sockets.front()->send_to(datagram, target.value());
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), datagram.size());
  auto poller = factory.network().pollers().create_poller();
  ASSERT_TRUE(poller);
  ASSERT_TRUE(poller.value());
  const auto ready =
      poller.value()->wait(requests, std::chrono::seconds{2}, {});
  ASSERT_TRUE(ready);
  ASSERT_EQ(ready.value().ready.size(), 1U);
  EXPECT_EQ(ready.value().ready.front().tag, socket_count);
}

TEST_F(SocketPolling, AZeroTimeoutStillReportsAReadySocket) {
  auto& network = factory.network();
  auto receiver =
      network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto sender = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  ASSERT_TRUE(receiver);
  ASSERT_TRUE(sender);
  ASSERT_TRUE(poller);
  ASSERT_TRUE(receiver.value()->bind({IpAddress::loopback_ipv4(), 0}));
  const auto endpoint = receiver.value()->local_endpoint();
  ASSERT_TRUE(endpoint);
  const std::array<std::byte, 4> payload{};
  ASSERT_TRUE(sender.value()->send_to(payload, endpoint.value()));
  const std::array<PollRequest, 1> requests{
      {{receiver.value().get(), 7, PollEvent::readable}}};
  const auto armed =
      poller.value()->wait(requests, std::chrono::milliseconds{500}, token);
  ASSERT_TRUE(armed);
  ASSERT_FALSE(armed.value().ready.empty());
  const auto immediate =
      poller.value()->wait(requests, std::chrono::nanoseconds::zero(), token);
  ASSERT_TRUE(immediate);
  ASSERT_EQ(immediate.value().ready.size(), 1U);
  EXPECT_EQ(immediate.value().ready.front().tag, 7U);
}

TEST_F(SocketPolling, ManyWakesCoalesceIntoOneAndAWakeFromAThreadEndsAWait) {
  auto poller = factory.network().pollers().create_poller();
  ASSERT_TRUE(poller);
  for (int round = 0; round < 3; ++round) {
    for (int wake = 0; wake < 100; ++wake)
      ASSERT_TRUE(poller.value()->wake()) << "round " << round;
    const auto woken =
        poller.value()->wait({}, std::chrono::milliseconds{500}, token);
    ASSERT_TRUE(woken) << "round " << round;
    EXPECT_TRUE(woken.value().woke) << "round " << round;
    const auto quiet =
        poller.value()->wait({}, std::chrono::milliseconds{20}, token);
    ASSERT_TRUE(quiet) << "round " << round;
    EXPECT_FALSE(quiet.value().woke) << "round " << round;
  }
  auto waker = libchiaking::platform::Thread::start(
      factory.threading(), "waker",
      libchiaking::platform::ThreadRole::unspecified, [&] {
        pause(std::chrono::milliseconds{30});
        static_cast<void>(poller.value()->wake());
      });
  ASSERT_TRUE(waker);
  const auto woken = poller.value()->wait({}, std::chrono::seconds{5}, token);
  static_cast<void>(waker.value().join());
  ASSERT_TRUE(woken);
  EXPECT_TRUE(woken.value().woke);
}

}  // namespace
}  // namespace platform_net_test
