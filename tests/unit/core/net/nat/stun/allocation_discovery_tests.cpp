#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/stun/mapping_estimate.hpp"
#include "src/core/net/nat/stun/public_servers.hpp"
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
namespace stun = libchiaking::net::nat::stun;
using Error = stun::StunAllocationDiscoveryError;

class SequentialRandom final : public pf::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void, pf::PlatformError> override {
    if (fail) return libchiaking::unexpected(pf::PlatformError::io_failure);
    for (auto& byte : output) byte = static_cast<std::byte>(next++);
    ++calls;
    return {};
  }

  std::uint8_t next{};
  unsigned calls{};
  bool fail{};
};

class ListHttp final : public pf::HttpProvider {
 public:
  auto perform(const pf::HttpRequest& request, const pf::StopToken&)
      -> libchiaking::Expected<pf::HttpResponse, pf::HttpError> override {
    ++calls;
    urls.emplace_back(request.url.view());
    EXPECT_TRUE(request.url.view() == stun::PublicServers::host_list_url ||
                request.url.view() == stun::PublicServers::ipv6_host_list_url)
        << request.url.view();
    if (fail) return libchiaking::unexpected(pf::HttpError::disconnected);
    if (request.url.view() == stun::PublicServers::ipv6_host_list_url)
      return ipv6_response.clone();
    return response.clone();
  }

  pf::HttpResponse response;
  pf::HttpResponse ipv6_response;
  std::vector<std::string> urls;
  bool fail{};
  unsigned calls{};
};

class OneHostDns final : public pf::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view)
      -> libchiaking::Expected<std::vector<pf::IpAddress>,
                               pf::NetError> override {
    calls.emplace_back(host);
    if (host == accepted_host)
      return std::vector{pf::IpAddress::loopback_ipv4()};
    return libchiaking::unexpected(pf::NetError::host_unreachable);
  }

  std::string accepted_host;
  std::vector<std::string> calls;
};

class StunAllocationDiscovery : public ::testing::Test {
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
    client_endpoint = client_ep.value();
    auto made_poller = network.pollers().create_poller();
    ASSERT_TRUE(made_poller);
    poller = std::move(made_poller.value());
    http.response.status_code = 200;
    const auto body = std::string{"preferred.example:"} +
                      std::to_string(peer_ep.value().port) + "\n";
    http.response.body.assign(std::string_view{body});
    http.ipv6_response.status_code = 200;
    const std::string ipv6_body{"[2001:db8::1]:3478\n"};
    http.ipv6_response.body.assign(std::string_view{ipv6_body});
    dns.accepted_host = "preferred.example";
  }

  auto answer_once() -> bool {
    constexpr std::array<std::byte, 32> response{
        std::byte{1},    std::byte{1},    std::byte{0},    std::byte{12},
        std::byte{0x21}, std::byte{0x12}, std::byte{0xa4}, std::byte{0x42},
        std::byte{0},    std::byte{1},    std::byte{2},    std::byte{3},
        std::byte{4},    std::byte{5},    std::byte{6},    std::byte{7},
        std::byte{8},    std::byte{9},    std::byte{10},   std::byte{11},
        std::byte{0},    std::byte{1},    std::byte{0},    std::byte{8},
        std::byte{0},    std::byte{1},    std::byte{0x23}, std::byte{0x45},
        std::byte{192},  std::byte{0},    std::byte{2},    std::byte{9}};
    return static_cast<bool>(peer->send_to(response, client_endpoint));
  }

  auto discover(SequentialRandom& source, const pf::StopToken& token,
                std::chrono::nanoseconds timeout = stun::stun_reply_timeout,
                stun::StunDiscoveryState* state = nullptr,
                std::chrono::nanoseconds budget = stun::stun_discovery_budget) {
    return stun::AllocationDiscovery::discover(
        *client, *poller, factory.clock(), source, http, dns, token, timeout,
        nullptr, state, budget);
  }

  static auto list_url() -> std::string {
    return std::string{stun::PublicServers::host_list_url};
  }

  static auto ipv6_list_url() -> std::string {
    return std::string{stun::PublicServers::ipv6_host_list_url};
  }

  pf::DefaultPlatformFactory factory;
  pf::NetworkProvider& network = factory.network();
  std::unique_ptr<pf::UdpSocket> client;
  std::unique_ptr<pf::UdpSocket> peer;
  std::unique_ptr<pf::Poller> poller;
  pf::SocketEndpoint client_endpoint{};
  ListHttp http;
  OneHostDns dns;
  SequentialRandom random;
  stun::StunDiscoveryState cache;
  const pf::StopToken stop;
};

TEST_F(StunAllocationDiscovery, FetchesBothListsAndReturnsTheMapping) {
  ASSERT_TRUE(answer_once());
  const auto mapped =
      discover(random, stop, std::chrono::milliseconds{100}, &cache);
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped.value().projected,
            (pf::SocketEndpoint{pf::IpAddress::ipv4({192, 0, 2, 9}), 0x2345}));
  EXPECT_EQ(mapped.value().increment, 0);
  EXPECT_EQ(http.calls, 2U);
  EXPECT_EQ(random.calls, 10U);
  EXPECT_EQ(http.urls, (std::vector<std::string>{list_url(), ipv6_list_url()}));
  ASSERT_EQ(cache.preferred_ipv6.size(), 1U);
  EXPECT_EQ(cache.preferred_ipv6[0].host, "2001:db8::1");
  EXPECT_EQ(cache.preferred_ipv6[0].port, 3478);
  ASSERT_FALSE(dns.calls.empty());
  EXPECT_EQ(dns.calls[0], "preferred.example");
}

TEST_F(StunAllocationDiscovery, ReusesTheCachedStepWithOneServer) {
  ASSERT_TRUE(answer_once());
  ASSERT_TRUE(discover(random, stop, std::chrono::milliseconds{100}, &cache));
  ASSERT_TRUE(cache.allocation);
  cache.allocation->increment = 3;
  cache.allocation->random_allocation = true;
  dns.calls.clear();
  random.next = 0;
  ASSERT_TRUE(answer_once());
  auto data_mapping =
      discover(random, stop, std::chrono::milliseconds{100}, &cache);
  ASSERT_TRUE(data_mapping);
  EXPECT_EQ(data_mapping.value().increment, 3);
  EXPECT_TRUE(data_mapping.value().random_allocation);
  EXPECT_EQ(data_mapping.value().projected.port, 0x2345);
  EXPECT_EQ(http.calls, 2U);
  EXPECT_EQ(dns.calls, std::vector<std::string>{"preferred.example"});
}

TEST_F(StunAllocationDiscovery, FallsBackToTheFixedServersWhenTheListFails) {
  http.fail = true;
  dns.accepted_host = "stun.moonlight-stream.org";
  const auto fallback = discover(random, stop, std::chrono::milliseconds{2});
  ASSERT_FALSE(fallback);
  EXPECT_EQ(fallback.error(), Error::exhausted);
  ASSERT_EQ(dns.calls.size(), 11U);
  EXPECT_EQ(dns.calls[0], "stun.moonlight-stream.org");
  EXPECT_EQ(http.urls, std::vector<std::string>{list_url()});
  EXPECT_EQ(random.calls, 10U);
}

TEST_F(StunAllocationDiscovery, StopsAskingOnceTheBudgetIsSpent) {
  http.fail = true;
  dns.accepted_host = "stun.moonlight-stream.org";
  const auto spent = discover(random, stop, std::chrono::milliseconds{20},
                              nullptr, std::chrono::milliseconds{10});
  ASSERT_FALSE(spent);
  EXPECT_EQ(spent.error(), Error::exhausted);
  ASSERT_EQ(dns.calls.size(), 1U);
  EXPECT_EQ(dns.calls[0], "stun.moonlight-stream.org");
}

TEST_F(StunAllocationDiscovery, ReportsNoServersWhenNoneResolves) {
  http.fail = true;
  dns.accepted_host.clear();
  const auto no_servers = discover(random, stop, std::chrono::milliseconds{2});
  ASSERT_FALSE(no_servers);
  EXPECT_EQ(no_servers.error(), Error::no_servers);
}

TEST_F(StunAllocationDiscovery, ReportsARandomnessFailure) {
  SequentialRandom failed_random;
  failed_random.fail = true;
  const auto random_failure =
      discover(failed_random, stop, std::chrono::milliseconds{2});
  ASSERT_FALSE(random_failure);
  EXPECT_EQ(random_failure.error(), Error::random_failure);
}

TEST_F(StunAllocationDiscovery, CarriesOnWithoutTheIpv6List) {
  http.ipv6_response.status_code = 404;
  stun::StunDiscoveryState ipv6_failed;
  ASSERT_TRUE(answer_once());
  const auto without_ipv6 =
      discover(random, stop, std::chrono::milliseconds{100}, &ipv6_failed);
  ASSERT_TRUE(without_ipv6);
  EXPECT_EQ(ipv6_failed.preferred.size(), 1U);
  EXPECT_TRUE(ipv6_failed.preferred_ipv6.empty());
  ASSERT_EQ(http.urls.size(), 2U);
  EXPECT_EQ(http.urls[1], ipv6_list_url());
}

TEST_F(StunAllocationDiscovery, SkipsTheIpv6ListAfterAnIncompleteList) {
  http.ipv6_response.status_code = 404;
  const std::string incomplete{"preferred.example\n"};
  http.response.body.assign(std::string_view{incomplete});
  static_cast<void>(discover(random, stop, std::chrono::milliseconds{2}));
  EXPECT_EQ(http.urls, std::vector<std::string>{list_url()});
}

TEST_F(StunAllocationDiscovery, StopsBeforeFetchingWhenCancelled) {
  const pf::StopToken cancelled{std::make_shared<test_support::Stopped>()};
  const auto before = http.calls;
  const auto stopped = discover(random, cancelled);
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), Error::cancelled);
  EXPECT_EQ(http.calls, before);
}

}  // namespace
}  // namespace nat_test
