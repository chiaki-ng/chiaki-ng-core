#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/net/sockets/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <iphlpapi.h>
#include <winsock2.h>
#else
#include <net/if.h>
#endif

namespace platform_net_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::SocketEndpoint;
using libchiaking::platform::UdpReceiveResult;

class ScopedLinkLocal : public SocketRig {
 protected:
  void SetUp() override {
    name = loopback_link_local_interface();
    if (!name)
      GTEST_SKIP() << "no loopback fe80::1 interface; socket checks skipped";
    text = "fe80::1%" + *name;
    const auto parsed = libchiaking::platform::parse_ip_address(text);
    ASSERT_TRUE(parsed.has_value());
    address = parsed.value();
  }

  auto loopback_link_local_interface() -> std::optional<std::string> {
    constexpr std::array<std::uint8_t, 16> link_local_one{
        0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const auto interfaces = factory.network().interfaces().enumerate();
    if (!interfaces) return std::nullopt;
    for (const auto& entry : interfaces.value()) {
      if (entry.loopback && entry.address.is_ipv6() &&
          entry.address.ipv6_bytes() == link_local_one)
        return entry.name;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto reading() -> std::chrono::microseconds {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(factory.clock().now_microseconds())};
  }

  std::optional<std::string> name;
  std::string text;
  IpAddress address;
};

TEST_F(ScopedLinkLocal, ANamedZoneResolvesToTheInterfaceIndex) {
  const auto index =
      static_cast<std::uint32_t>(::if_nametoindex(name->c_str()));
  EXPECT_NE(index, 0U);
  EXPECT_EQ(address.scope_id(), index);
#if !defined(_WIN32)
  EXPECT_EQ(libchiaking::platform::to_scoped_string(address), text);
#endif
}

TEST_F(ScopedLinkLocal, DnsKeepsTheScopeId) {
  auto& dns = factory.network().dns();
  const auto plain = dns.resolve(text, "9295");
  ASSERT_TRUE(plain.has_value());
  ASSERT_FALSE(plain.value().empty());
  EXPECT_EQ(plain.value().front().scope_id(), address.scope_id());
  const auto cancellable =
      dns.resolve(text, "9295", libchiaking::platform::StopToken{});
  ASSERT_TRUE(cancellable.has_value());
  ASSERT_FALSE(cancellable.value().empty());
  EXPECT_EQ(cancellable.value().front().scope_id(), address.scope_id());
}

TEST_F(ScopedLinkLocal, UdpTravelsOverTheScopedAddress) {
  auto receiver = sockets().create_udp_socket(address);
  auto sender = sockets().create_udp_socket(address);
  ASSERT_TRUE(receiver.has_value());
  ASSERT_TRUE(sender.has_value());
  ASSERT_TRUE(receiver.value()->bind({address, 0}).has_value());
  const auto local = receiver.value()->local_endpoint();
  ASSERT_TRUE(local.has_value());
  EXPECT_EQ(local.value().address, address);
  const std::array<std::byte, 4> payload{std::byte{1}, std::byte{2},
                                         std::byte{3}, std::byte{4}};
  const auto sent = sender.value()->send_to(payload, local.value());
  ASSERT_TRUE(sent.has_value());
  EXPECT_EQ(sent.value(), payload.size());
  std::array<std::byte, 16> buffer{};
  std::optional<UdpReceiveResult> received;
  const auto start = reading();
  while (reading() - start < std::chrono::seconds{2}) {
    auto result = receiver.value()->receive_from(buffer);
    if (result) {
      received = result.value();
      break;
    }
    pause(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->size, payload.size());
  EXPECT_EQ(received->source.address.scope_id(), address.scope_id());

  auto connected = sockets().create_udp_socket(address);
  ASSERT_TRUE(connected.has_value());
  EXPECT_TRUE(connected.value()->connect(local.value()).has_value());
  EXPECT_TRUE(connected.value()->send(payload).has_value());
}

TEST_F(ScopedLinkLocal, TcpConnectsOverTheScopedAddress) {
  auto listener = sockets().create_tcp_listener(address);
  ASSERT_TRUE(listener.has_value());
  EXPECT_TRUE(listener.value()->bind({address, 0}).has_value());
  EXPECT_TRUE(listener.value()->listen(1).has_value());
  const auto local = listener.value()->local_endpoint();
  ASSERT_TRUE(local.has_value());
  auto client = sockets().create_tcp_socket(address);
  ASSERT_TRUE(client.has_value());
  const auto connected = client.value()->connect(local.value());
  if (!connected) {
    EXPECT_EQ(connected.error(), NetError::would_block);
  }
  std::optional<SocketEndpoint> peer;
  const auto start = reading();
  while (reading() - start < std::chrono::seconds{2}) {
    const auto completed = client.value()->complete_connect();
    if (!completed && completed.error() != NetError::would_block) break;
    const auto current = client.value()->peer_endpoint();
    if (current) {
      peer = current.value();
      break;
    }
    pause(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(peer.has_value());
  EXPECT_EQ(peer->address, address);
}

}  // namespace
}  // namespace platform_net_test
