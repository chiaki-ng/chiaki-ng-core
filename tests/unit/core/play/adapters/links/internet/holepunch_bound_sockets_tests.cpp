#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/holepunch_probe_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using holepunch_probe::QuietUdp;

enum class Failure { none, create, null_socket, option, bind, endpoint };

struct SocketScript {
  Failure v4{Failure::none};
  Failure v6{Failure::none};
  unsigned v4_created{};
  unsigned v6_created{};
  unsigned destroyed{};
  unsigned ttl_calls{};
  unsigned fail_create_after{};
  std::uint16_t v6_bound_port{};
  bool v6_wrong_family{};
  bool v6_wrong_port{};
};

class ScriptedUdp final : public QuietUdp {
 public:
  ScriptedUdp(pf::NetworkProvider& network, SocketScript& script, bool ipv6,
              Failure failure) noexcept
      : QuietUdp(network), script_(script), ipv6_(ipv6), failure_(failure) {}
  ~ScriptedUdp() override { ++script_.destroyed; }

  ScriptedUdp(const ScriptedUdp&) = delete;
  auto operator=(const ScriptedUdp&) -> ScriptedUdp& = delete;

  [[nodiscard]] auto bind(const pf::SocketEndpoint& endpoint)
      -> libchiaking::Expected<void, pf::NetError> override {
    if (failure_ == Failure::bind)
      return libchiaking::unexpected(pf::NetError::address_in_use);
    port_ = endpoint.port == 0 ? 49152 : endpoint.port;
    if (ipv6_) script_.v6_bound_port = endpoint.port;
    return {};
  }
  [[nodiscard]] auto set_shared_port_binding(bool enabled)
      -> libchiaking::Expected<void, pf::NetError> override {
    EXPECT_TRUE(enabled);
    if (failure_ == Failure::option)
      return libchiaking::unexpected(pf::NetError::not_supported);
    return {};
  }
  [[nodiscard]] auto set_ipv4_ttl(std::uint8_t ttl)
      -> libchiaking::Expected<void, pf::NetError> override {
    EXPECT_EQ(ttl, 2);
    ++script_.ttl_calls;
    return {};
  }
  [[nodiscard]] auto local_endpoint()
      -> libchiaking::Expected<pf::SocketEndpoint, pf::NetError> override {
    if (failure_ == Failure::endpoint)
      return libchiaking::unexpected(pf::NetError::invalid_argument);
    const auto address = ipv6_ && !script_.v6_wrong_family
                             ? pf::IpAddress::any_ipv6()
                             : pf::IpAddress::any_ipv4();
    return pf::SocketEndpoint{
        address, static_cast<std::uint16_t>(
                     port_ + (ipv6_ && script_.v6_wrong_port ? 1U : 0U))};
  }

 private:
  SocketScript& script_;
  bool ipv6_{};
  Failure failure_{};
  std::uint16_t port_{};
};

class ScriptedSockets final : public pf::SocketProvider {
 public:
  [[nodiscard]] auto create_udp_socket(const pf::IpAddress& family)
      -> libchiaking::Expected<std::unique_ptr<pf::UdpSocket>,
                               pf::NetError> override {
    const bool ipv6 = family.is_ipv6();
    if (ipv6)
      ++script.v6_created;
    else
      ++script.v4_created;
    const auto failure = ipv6 ? script.v6 : script.v4;
    if (failure == Failure::create ||
        (script.fail_create_after &&
         script.v4_created > script.fail_create_after))
      return libchiaking::unexpected(pf::NetError::not_supported);
    if (failure == Failure::null_socket)
      return std::unique_ptr<pf::UdpSocket>{};
    return std::unique_ptr<pf::UdpSocket>{
        std::make_unique<ScriptedUdp>(network_, script, ipv6, failure)};
  }
  [[nodiscard]] auto create_tcp_socket(const pf::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<pf::TcpSocket>,
                               pf::NetError> override {
    return libchiaking::unexpected(pf::NetError::not_supported);
  }
  [[nodiscard]] auto create_tcp_listener(const pf::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<pf::TcpListener>,
                               pf::NetError> override {
    return libchiaking::unexpected(pf::NetError::not_supported);
  }

  SocketScript script{};

 private:
  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(), test_support::LiveProviders::steady()};
};

class HolepunchBoundSockets : public ::testing::Test {
 protected:
  static auto dropped(Failure failure) -> bool {
    return failure != Failure::create && failure != Failure::null_socket;
  }

  static constexpr Failure failures[] = {Failure::create, Failure::null_socket,
                                         Failure::option, Failure::bind,
                                         Failure::endpoint};

  ScriptedSockets sockets_;
};

TEST_F(HolepunchBoundSockets, KeepsTheNatSocketsCreatedBeforeAFailure) {
  sockets_.script.fail_create_after = 3;
  nat::BoundSockets bound;
  nat::BoundSockets::add_nat_sockets(bound, sockets_, 250, {});
  EXPECT_EQ(bound.probes.size(), 3U);
  EXPECT_EQ(sockets_.script.v4_created, 4U);
  EXPECT_EQ(sockets_.script.ttl_calls, 3U);
  bound.probes.clear();
  EXPECT_EQ(sockets_.script.destroyed, 3U);
}

TEST_F(HolepunchBoundSockets, DropsNatSocketsThatCannotBind) {
  sockets_.script.v4 = Failure::bind;
  nat::BoundSockets bound;
  nat::BoundSockets::add_nat_sockets(bound, sockets_, 4, {});
  EXPECT_TRUE(bound.probes.empty());
  EXPECT_EQ(sockets_.script.destroyed, 4U);
}

TEST_F(HolepunchBoundSockets, BindsBothFamiliesToOnePort) {
  auto bound = nat::BoundSockets::create(sockets_, true);
  ASSERT_TRUE(bound.has_value());
  EXPECT_TRUE(bound.value().ipv4);
  EXPECT_TRUE(bound.value().ipv6);
  EXPECT_EQ(bound.value().local_port, 49152U);
  EXPECT_EQ(sockets_.script.v6_bound_port, 49152U);
  EXPECT_EQ(sockets_.script.destroyed, 0U);
}

TEST_F(HolepunchBoundSockets, CarriesOnWithIpv4WhenIpv6Fails) {
  for (const auto failure : failures) {
    SCOPED_TRACE(static_cast<int>(failure));
    ScriptedSockets sockets;
    sockets.script.v6 = failure;
    auto bound = nat::BoundSockets::create(sockets, true);
    ASSERT_TRUE(bound.has_value());
    EXPECT_TRUE(bound.value().ipv4);
    EXPECT_FALSE(bound.value().ipv6);
    EXPECT_EQ(bound.value().local_port, 49152U);
    EXPECT_EQ(sockets.script.v6_created, 1U);
    EXPECT_EQ(sockets.script.destroyed, dropped(failure) ? 1U : 0U);
    bound.value().ipv4.reset();
    EXPECT_EQ(sockets.script.destroyed, dropped(failure) ? 2U : 1U);
  }
}

TEST_F(HolepunchBoundSockets, FailsWithoutTryingIpv6WhenIpv4Fails) {
  for (const auto failure : failures) {
    SCOPED_TRACE(static_cast<int>(failure));
    ScriptedSockets sockets;
    sockets.script.v4 = failure;
    auto bound = nat::BoundSockets::create(sockets, true);
    EXPECT_FALSE(bound.has_value());
    EXPECT_EQ(sockets.script.v4_created, 1U);
    EXPECT_EQ(sockets.script.v6_created, 0U);
    EXPECT_EQ(sockets.script.destroyed, dropped(failure) ? 1U : 0U);
  }
}

TEST_F(HolepunchBoundSockets, DropsIpv6BoundToAnotherPort) {
  sockets_.script.v6_wrong_port = true;
  auto bound = nat::BoundSockets::create(sockets_, true);
  ASSERT_TRUE(bound.has_value());
  EXPECT_FALSE(bound.value().ipv6);
}

TEST_F(HolepunchBoundSockets, DropsIpv6BoundToAnotherFamily) {
  sockets_.script.v6_wrong_family = true;
  auto bound = nat::BoundSockets::create(sockets_, true);
  ASSERT_TRUE(bound.has_value());
  EXPECT_FALSE(bound.value().ipv6);
}

TEST_F(HolepunchBoundSockets, LeavesIpv6AloneWhenNotAsked) {
  auto bound = nat::BoundSockets::create(sockets_, false);
  ASSERT_TRUE(bound.has_value());
  EXPECT_TRUE(bound.value().ipv4);
  EXPECT_FALSE(bound.value().ipv6);
  EXPECT_EQ(sockets_.script.v6_created, 0U);
}

}  // namespace
}  // namespace play_test
