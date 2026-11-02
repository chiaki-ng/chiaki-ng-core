#include "libchiaking/platform/default_factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace pf = libchiaking::platform;
using ProbeError = internet::holepunch::OfferProbeError;

class PeerOnlyDns final : public pf::DnsProvider {
 public:
  auto resolve(std::string_view host, std::string_view)
      -> libchiaking::Expected<std::vector<pf::IpAddress>,
                               pf::NetError> override {
    ++calls;
    if (host == "peer") return std::vector{pf::IpAddress::loopback_ipv4()};
    return libchiaking::unexpected(pf::NetError::host_unreachable);
  }

  int calls{};
};

class RampRandom final : public pf::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> bytes)
      -> libchiaking::Expected<void, pf::PlatformError> override {
    ++calls;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::byte>(0xA0U + i);
    return {};
  }

  int calls{};
};

class HolepunchOfferProbe : public ::testing::Test {
 protected:
  void SetUp() override {
    auto poller = network_.pollers().create_poller();
    ASSERT_TRUE(poller);
    poller_ = std::move(poller.value());
    local_.sid = 1;
    console_.sid = 2;
    console_.candidates = {candidate("dead", 1000), candidate("peer", 9)};
  }

  static auto candidate(std::string_view host, std::uint16_t port)
      -> nat::Candidate {
    nat::Candidate out{};
    out.addr = host;
    out.port = port;
    return out;
  }

  auto sockets() -> nat::BoundSockets {
    auto sockets = nat::BoundSockets::create(network_.sockets(), false);
    EXPECT_TRUE(sockets);
    if (!sockets) return {};
    return std::move(sockets.value());
  }

  auto probe(nat::BoundSockets sockets, const pf::StopToken& stop,
             nat::ProbeRetryPolicy retry = {}) {
    return internet::holepunch::Offer::probe_peer(
        local_, console_, std::move(sockets), dns_, *poller_, factory_.clock(),
        random_, stop, retry);
  }

  pf::DefaultPlatformFactory factory_;
  pf::NetworkProvider& network_ = factory_.network();
  std::unique_ptr<pf::Poller> poller_;
  nat::ConnRequest local_{};
  nat::ConnRequest console_{};
  PeerOnlyDns dns_;
  RampRandom random_;
  const pf::StopToken open_;
};

TEST_F(HolepunchOfferProbe, SelectsTheCandidateThatAnswered) {
  auto peer =
      network_.sockets().create_udp_socket(pf::IpAddress::loopback_ipv4());
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer.value());
  ASSERT_TRUE(peer.value()->bind({pf::IpAddress::loopback_ipv4(), 0}));
  auto peer_ep = peer.value()->local_endpoint();
  ASSERT_TRUE(peer_ep);
  auto bound = sockets();
  ASSERT_TRUE(bound.ipv4);
  auto* owned = bound.ipv4.get();
  nat::CandidateProbe response_profile{};
  response_profile.request_id = {std::byte{0xA0}, std::byte{0xA1},
                                 std::byte{0xA2}, std::byte{0xA3},
                                 std::byte{0xA4}};
  const pf::SocketEndpoint to{pf::IpAddress::loopback_ipv4(), bound.local_port};
  const auto response = nat::ProbeCodec::encode_response(response_profile, {});
  const auto sent = peer.value()->send_to(response, to);
  ASSERT_TRUE(sent);
  ASSERT_EQ(sent.value(), response.size());
  const auto followup = nat::ProbeCodec::encode(response_profile);
  ASSERT_TRUE(peer.value()->send_to(followup, to));
  console_.candidates[1].port = peer_ep.value().port;

  auto winner = probe(std::move(bound), open_, {0, std::chrono::seconds{2}});
  ASSERT_TRUE(winner);
  EXPECT_EQ(winner.value().socket.get(), owned);
  EXPECT_EQ(winner.value().candidate_index, 1U);
  EXPECT_EQ(winner.value().peer, peer_ep.value());
  EXPECT_EQ(dns_.calls, 2);
  EXPECT_EQ(random_.calls, 1);
}

TEST_F(HolepunchOfferProbe, RefusesOurOfferWithASessionIdBeyondSixteenBits) {
  local_.sid = 65536;
  auto invalid = probe(sockets(), open_);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), ProbeError::invalid_local_offer);
  EXPECT_EQ(dns_.calls, 0);
  EXPECT_EQ(random_.calls, 0);
}

TEST_F(HolepunchOfferProbe, RefusesAPeerOfferWithoutCandidates) {
  console_.candidates.clear();
  auto none = probe(sockets(), open_);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), ProbeError::invalid_peer_offer);
}

TEST_F(HolepunchOfferProbe, StopsBeforeResolvingWhenCancelled) {
  pf::StopToken stopped{std::make_shared<test_support::Stopped>()};
  auto cancelled = probe(sockets(), stopped);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), ProbeError::cancelled);
  EXPECT_EQ(dns_.calls, 0);
  EXPECT_EQ(random_.calls, 0);
}

TEST_F(HolepunchOfferProbe, TimesOutWhenNoCandidateAnswers) {
  auto timeout = probe(sockets(), open_, {0, std::chrono::nanoseconds{0}});
  ASSERT_FALSE(timeout);
  EXPECT_EQ(timeout.error(), ProbeError::wait_timeout);
}

}  // namespace
}  // namespace play_test
