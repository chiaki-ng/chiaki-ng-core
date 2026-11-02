#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nat_test {
namespace {

namespace pf = libchiaking::platform;
namespace holepunch = libchiaking::net::nat::holepunch;
namespace stun = libchiaking::net::nat::stun;

class GatherInterfaces final : public pf::InterfacesProvider {
 public:
  auto enumerate() -> libchiaking::Expected<std::vector<pf::InterfaceInfo>,
                                            pf::NetError> override {
    if (fail) return libchiaking::unexpected(pf::NetError::system_failure);
    pf::InterfaceInfo info{};
    info.address = pf::IpAddress::ipv4({192, 168, 1, 42});
    info.operational = true;
    return std::vector{info};
  }

  bool fail{};
};

class GatherHttp final : public pf::HttpProvider {
 public:
  auto perform(const pf::HttpRequest&, const pf::StopToken&)
      -> libchiaking::Expected<pf::HttpResponse, pf::HttpError> override {
    ++calls;
    return response.clone();
  }

  pf::HttpResponse response;
  unsigned calls{};
};

class GatherDns final : public pf::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view)
      -> libchiaking::Expected<std::vector<pf::IpAddress>,
                               pf::NetError> override {
    if (!fail && host == "preferred.example")
      return std::vector{pf::IpAddress::loopback_ipv4()};
    return libchiaking::unexpected(pf::NetError::host_unreachable);
  }

  bool fail{};
};

class GatherRandom final : public pf::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> bytes)
      -> libchiaking::Expected<void, pf::PlatformError> override {
    for (auto& byte : bytes) byte = static_cast<std::byte>(next++);
    return {};
  }

  std::uint8_t next{};
};

class GatherGateway final : public pf::GatewaySession {
 public:
  auto local_address() const -> pf::IpAddress override {
    return pf::IpAddress::ipv4({192, 0, 2, 10});
  }

  auto map_udp(std::uint16_t port, const pf::StopToken&)
      -> libchiaking::Expected<pf::SocketEndpoint, pf::GatewayError> override {
    ++maps;
    if (fail) return libchiaking::unexpected(pf::GatewayError::network);
    return pf::SocketEndpoint{pf::IpAddress::ipv4({203, 0, 113, 20}), port};
  }

  auto close() -> libchiaking::Expected<void, pf::GatewayError> override {
    ++closes;
    return {};
  }

  bool fail{};
  unsigned maps{};
  unsigned closes{};
};

class HolepunchGather : public ::testing::Test {
 protected:
  void SetUp() override {
    auto made_client =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    auto made_peer =
        network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
    ASSERT_TRUE(made_client);
    ASSERT_TRUE(made_peer);
    client = std::move(made_client.value());
    peer = std::move(made_peer.value());
    ASSERT_TRUE(client->bind({pf::IpAddress::loopback_ipv4(), 0}));
    ASSERT_TRUE(peer->bind({pf::IpAddress::loopback_ipv4(), 0}));
    const auto client_ep = client->local_endpoint();
    const auto peer_ep = peer->local_endpoint();
    ASSERT_TRUE(client_ep);
    ASSERT_TRUE(peer_ep);
    client_port = client_ep.value().port;
    client_endpoint = client_ep.value();
    auto made_poller = network.pollers().create_poller();
    ASSERT_TRUE(made_poller);
    poller = std::move(made_poller.value());
    http.response.status_code = 200;
    const auto text = std::string{"preferred.example:"} +
                      std::to_string(peer_ep.value().port) + "\n";
    http.response.body.assign(std::string_view{text});
    mapped_port =
        static_cast<std::uint16_t>(client_port == 40000U ? 40001U : 40000U);
  }

  auto binding_response() const -> std::array<std::byte, 32> {
    return {std::byte{1},
            std::byte{1},
            std::byte{0},
            std::byte{12},
            std::byte{0x21},
            std::byte{0x12},
            std::byte{0xa4},
            std::byte{0x42},
            std::byte{0},
            std::byte{1},
            std::byte{2},
            std::byte{3},
            std::byte{4},
            std::byte{5},
            std::byte{6},
            std::byte{7},
            std::byte{8},
            std::byte{9},
            std::byte{10},
            std::byte{11},
            std::byte{0},
            std::byte{1},
            std::byte{0},
            std::byte{8},
            std::byte{0},
            std::byte{1},
            static_cast<std::byte>(mapped_port >> 8U),
            static_cast<std::byte>(mapped_port & 0xffU),
            std::byte{203},
            std::byte{0},
            std::byte{113},
            std::byte{7}};
  }

  auto answer_binding() -> bool {
    const auto response = binding_response();
    return static_cast<bool>(peer->send_to(response, client_endpoint));
  }

  auto gather(pf::UdpSocket& socket, const pf::StopToken& token,
              std::chrono::nanoseconds timeout = stun::stun_reply_timeout,
              pf::GatewaySession* session = nullptr) {
    stun::StunDiscoveryState discovery;
    return holepunch::CandidateGathering::gather(
        {socket, *poller, interfaces, factory.clock(), random, http, dns, false,
         75U, timeout, session},
        token, discovery);
  }

  pf::DefaultPlatformFactory factory;
  pf::NetworkProvider& network = factory.network();
  std::unique_ptr<pf::UdpSocket> client;
  std::unique_ptr<pf::UdpSocket> peer;
  std::unique_ptr<pf::Poller> poller;
  pf::SocketEndpoint client_endpoint{};
  std::uint16_t client_port{};
  std::uint16_t mapped_port{};
  GatherHttp http;
  GatherDns dns;
  GatherInterfaces interfaces;
  GatherRandom random;
  GatherGateway gateway;
  const pf::StopToken stop;
};

TEST_F(HolepunchGather, OffersTheMappedStaticAndLocalCandidates) {
  ASSERT_TRUE(answer_binding());
  const auto gathered = gather(*client, stop, std::chrono::milliseconds{100});
  ASSERT_TRUE(gathered);
  const auto& candidates = gathered.value().offer_candidates;
  ASSERT_EQ(candidates.size(), 3U);
  EXPECT_EQ(candidates[0].type, holepunch::CandidateType::stun);
  EXPECT_EQ(candidates[0].addr, "203.0.113.7");
  EXPECT_EQ(candidates[0].port, mapped_port);
  EXPECT_EQ(candidates[1].type, holepunch::CandidateType::static_);
  EXPECT_EQ(candidates[1].port, client_port);
  EXPECT_EQ(candidates[2].type, holepunch::CandidateType::local);
  EXPECT_EQ(candidates[2].addr, "192.168.1.42");
  EXPECT_EQ(candidates[2].port, client_port);
  EXPECT_EQ(gathered.value().local.addr, "192.168.1.42");
  EXPECT_EQ(gathered.value().local.port, client_port);
  EXPECT_EQ(
      gathered.value().allocation.projected,
      (pf::SocketEndpoint{pf::IpAddress::ipv4({203, 0, 113, 7}), mapped_port}));
  EXPECT_EQ(gathered.value().allocation.increment, 0);
  EXPECT_FALSE(gathered.value().allocation.random_allocation);
  EXPECT_EQ(http.calls, 2U);
}

TEST_F(HolepunchGather, UsesAGatewayMappingInPlaceOfStun) {
  interfaces.fail = true;
  auto gateway_result =
      gather(*client, stop, std::chrono::milliseconds{100}, &gateway);
  ASSERT_TRUE(gateway_result);
  ASSERT_EQ(gateway_result.value().offer_candidates.size(), 2U);
  EXPECT_EQ(gateway_result.value().local.addr, "192.0.2.10");
  EXPECT_EQ(gateway_result.value().offer_candidates[0].addr, "203.0.113.20");
  EXPECT_EQ(gateway_result.value().offer_candidates[0].port, client_port);
  EXPECT_EQ(gateway.maps, 1U);
  EXPECT_EQ(http.calls, 0U);
}

TEST_F(HolepunchGather, FallsBackToStunWhenTheGatewayCannotMap) {
  interfaces.fail = true;
  gateway.fail = true;
  ASSERT_TRUE(answer_binding());
  auto fallback =
      gather(*client, stop, std::chrono::milliseconds{100}, &gateway);
  ASSERT_TRUE(fallback);
  ASSERT_FALSE(fallback.value().offer_candidates.empty());
  EXPECT_EQ(fallback.value().offer_candidates[0].addr, "203.0.113.7");
  EXPECT_EQ(fallback.value().local.addr, "192.0.2.10");
  EXPECT_EQ(gateway.maps, 1U);
  EXPECT_EQ(http.calls, 2U);
}

TEST_F(HolepunchGather, ReportsAMissingLocalCandidate) {
  interfaces.fail = true;
  const auto local_error = gather(*client, stop);
  ASSERT_FALSE(local_error);
  EXPECT_EQ(local_error.error(), holepunch::GatherError::local);
}

TEST_F(HolepunchGather, ReportsASocketWithoutAPortBeforeItsInterfaces) {
  interfaces.fail = true;
  auto unbound =
      network.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
  ASSERT_TRUE(unbound);
  const auto socket_error = gather(*unbound.value(), stop);
  ASSERT_FALSE(socket_error);
  EXPECT_EQ(socket_error.error(), holepunch::GatherError::socket);
}

TEST_F(HolepunchGather, OffersOnlyTheLocalAddressWhenNoStunServerAnswers) {
  dns.fail = true;
  const auto unmapped = gather(*client, stop);
  ASSERT_TRUE(unmapped);
  EXPECT_EQ(unmapped.value().local.addr, "192.168.1.42");
  EXPECT_EQ(pf::to_string(unmapped.value().allocation.projected.address),
            "192.168.1.42");
  EXPECT_EQ(unmapped.value().allocation.projected.port,
            unmapped.value().local.port);
  ASSERT_EQ(unmapped.value().offer_candidates.size(), 2U);
  EXPECT_EQ(unmapped.value().offer_candidates[0].type,
            holepunch::CandidateType::static_);
  EXPECT_EQ(unmapped.value().offer_candidates[0].addr, "192.168.1.42");
  EXPECT_EQ(unmapped.value().offer_candidates[1].type,
            holepunch::CandidateType::local);
}

TEST_F(HolepunchGather, StopsBeforeAskingForServersWhenCancelled) {
  dns.fail = true;
  const pf::StopToken cancelled{std::make_shared<test_support::Stopped>()};
  const auto calls_before = http.calls;
  const auto stopped = gather(*client, cancelled);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), holepunch::GatherError::cancelled);
  EXPECT_EQ(http.calls, calls_before);
}

}  // namespace
}  // namespace nat_test
