#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>

namespace platform_posix_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::PollEvent;
using libchiaking::platform::PollRequest;
using libchiaking::platform::SocketEndpoint;
using libchiaking::platform::StopToken;
using libchiaking::platform::UdpSocket;

class PosixDescriptors : public ::testing::Test {
 protected:
  [[nodiscard]] static auto open_descriptors() -> std::vector<int> {
    std::vector<int> descriptors;
    for (int descriptor = 0; descriptor < 4096; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) != -1) descriptors.push_back(descriptor);
    return descriptors;
  }

  [[nodiscard]] static auto contains(const std::vector<int>& values, int value)
      -> bool {
    return std::ranges::find(values, value) != values.end();
  }

  libchiaking::platform::DefaultPlatformFactory factory;
};

TEST_F(PosixDescriptors, NewDescriptorsCloseOnExec) {
  const auto before = open_descriptors();
  auto& network = factory.network();
  auto udp = network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  auto listener =
      network.sockets().create_tcp_listener(IpAddress::loopback_ipv4());
  auto client = network.sockets().create_tcp_socket(IpAddress::loopback_ipv4());
  auto poller = network.pollers().create_poller();
  ASSERT_TRUE(udp);
  ASSERT_TRUE(listener);
  ASSERT_TRUE(client);
  ASSERT_TRUE(poller);
  ASSERT_TRUE(
      listener.value()->bind(SocketEndpoint{IpAddress::loopback_ipv4(), 0}));
  ASSERT_TRUE(listener.value()->listen(4));
  const auto endpoint = listener.value()->local_endpoint();
  ASSERT_TRUE(endpoint);
  const auto connected = client.value()->connect(endpoint.value());
  if (!connected) {
    ASSERT_EQ(connected.error(), NetError::would_block);
  }
  const std::array<PollRequest, 1> requests{
      {{client.value().get(), 1, PollEvent::writable}}};
  ASSERT_TRUE(poller.value()->wait(requests, std::chrono::milliseconds{500},
                                   StopToken{}));
  libchiaking::platform::Sleeper sleeper{factory.threading(), factory.clock()};
  auto accepted = listener.value()->accept();
  for (int attempt = 0; !accepted && attempt < 200; ++attempt) {
    static_cast<void>(sleeper.sleep_for(std::chrono::milliseconds{5}));
    accepted = listener.value()->accept();
  }
  ASSERT_TRUE(accepted);
  const auto after = open_descriptors();
  int created = 0;
  for (const int descriptor : after) {
    if (contains(before, descriptor)) continue;
    ++created;
    EXPECT_NE(::fcntl(descriptor, F_GETFD) & FD_CLOEXEC, 0)
        << "descriptor " << descriptor << " is inheritable";
  }
  EXPECT_GE(created, 4);
}

TEST_F(PosixDescriptors, ExhaustionIsReportedAsResourceExhausted) {
  auto& sockets = factory.network().sockets();
  rlimit original{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &original), 0);
  const auto open = open_descriptors();
  ASSERT_FALSE(open.empty());
  rlimit lowered = original;
  lowered.rlim_cur = static_cast<rlim_t>(open.back() + 1);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
  std::vector<std::unique_ptr<UdpSocket>> held;
  libchiaking::Expected<std::unique_ptr<UdpSocket>, NetError> created =
      libchiaking::unexpected(NetError::system_failure);
  for (int attempt = 0; attempt < 8; ++attempt) {
    created = sockets.create_udp_socket(IpAddress::loopback_ipv4());
    if (!created) break;
    held.push_back(std::move(created.value()));
  }
  held.clear();
  static_cast<void>(::setrlimit(RLIMIT_NOFILE, &original));
  ASSERT_FALSE(created);
  EXPECT_EQ(created.error(), NetError::resource_exhausted);
}

}  // namespace
}  // namespace platform_posix_test
