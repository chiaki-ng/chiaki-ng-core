#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/net/nat/stun/binding_client.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/links/internet/holepunch_probe_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using holepunch_probe::QuietUdp;

class ScriptUdp final : public QuietUdp {
 public:
  using QuietUdp::QuietUdp;

  [[nodiscard]] auto send_to(std::span<const std::byte> bytes,
                             const pf::SocketEndpoint& destination)
      -> libchiaking::Expected<std::size_t, pf::NetError> override {
    ++sends;
    sent.emplace_back(bytes.begin(), bytes.end());
    if (all_sends_fail || destination.port == 4000 ||
        (fail_after && sends > fail_after))
      return libchiaking::unexpected(pf::NetError::network_unreachable);
    return bytes.size();
  }
  [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
      -> libchiaking::Expected<pf::UdpReceiveResult, pf::NetError> override {
    ++receives;
    if (receive_failure)
      return libchiaking::unexpected(pf::NetError::connection_reset);
    if (receives == 1 || always_would_block)
      return libchiaking::unexpected(pf::NetError::would_block);
    if (!replies.empty()) {
      auto next = std::move(replies.front());
      replies.erase(replies.begin());
      std::ranges::copy(next, buffer.begin());
      return pf::UdpReceiveResult{next.size(), peer, std::nullopt};
    }
    if (scripted_only)
      return libchiaking::unexpected(pf::NetError::would_block);
    std::ranges::copy(reply, buffer.begin());
    return pf::UdpReceiveResult{reply.size(), peer, std::nullopt};
  }
  [[nodiscard]] auto set_ipv4_ttl(std::uint8_t)
      -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }
  [[nodiscard]] auto local_endpoint()
      -> libchiaking::Expected<pf::SocketEndpoint, pf::NetError> override {
    return pf::SocketEndpoint{pf::IpAddress::loopback_ipv4(), 3000};
  }

  unsigned sends{};
  unsigned receives{};
  unsigned fail_after{};
  bool all_sends_fail{};
  bool receive_failure{};
  bool always_would_block{};
  bool scripted_only{};
  std::vector<std::vector<std::byte>> replies;
  std::vector<std::vector<std::byte>> sent;
  pf::SocketEndpoint peer{pf::IpAddress::loopback_ipv4(), 4001};
  std::vector<std::byte> reply;
};

class SteppingPoller final : public pf::Poller {
 public:
  SteppingPoller(pf::NetworkProvider& network, test_support::FakeClock& clock)
      : network_(network), clock_(clock) {}

  const pf::NetworkProvider& owner() const noexcept override {
    return network_;
  }
  auto wake() -> libchiaking::Expected<void, pf::NetError> override {
    return {};
  }
  auto wait(std::span<const pf::PollRequest> requests, std::chrono::nanoseconds,
            const pf::StopToken& stop)
      -> libchiaking::Expected<pf::PollWaitResult, pf::NetError> override {
    clock_.now_us += step;
    pf::PollWaitResult result;
    result.stop_requested = stop.stop_requested();
    for (const auto& request : requests)
      result.ready.push_back({request.tag, pf::PollEvent::readable});
    return result;
  }

  std::uint64_t step{10};

 private:
  pf::NetworkProvider& network_;
  test_support::FakeClock& clock_;
};

class HolepunchProbeWait : public ::testing::Test {
 protected:
  static auto profile() -> nat::CandidateProbe {
    nat::CandidateProbe profile{};
    profile.request_id = {std::byte{1}, std::byte{2}, std::byte{3},
                          std::byte{4}, std::byte{5}};
    return profile;
  }

  auto wait(std::span<const nat::ProbeSocketGroup> groups,
            const nat::CandidateProbe& profile, bool* answered = nullptr) {
    const auto request = nat::ProbeCodec::encode(profile);
    return nat::Probe::wait_for_response_from_groups(
        groups, poller_, clock_, request, profile.request_id, profile, {},
        std::chrono::microseconds{100}, answered);
  }

  auto wait_with_retries(std::span<const nat::ProbeSocketGroup> groups,
                         const nat::CandidateProbe& profile) {
    const auto request = nat::ProbeCodec::encode(profile);
    nat::ProbeRetryPolicy policy;
    policy.max_retries = 3;
    policy.attempt_timeout = std::chrono::microseconds{100};
    return nat::Probe::wait_for_response_from_groups_with_retries(
        groups, poller_, clock_, request, profile.request_id, profile, {},
        policy);
  }

  auto two_peer_wait(ScriptUdp& socket) {
    const auto probe = profile();
    auto response = nat::ProbeCodec::encode_response(probe, socket.peer);
    socket.reply.assign(response.begin(), response.end());
    const pf::SocketEndpoint peers[] = {{socket.peer.address, 4000},
                                        socket.peer};
    const nat::ProbeSocketGroup groups[] = {{&socket, peers}};
    auto result = wait(groups, probe);
    EXPECT_EQ(socket.sends, 2U);
    return result;
  }

  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(),
                                      test_support::LiveProviders::steady()};
  test_support::FakeClock clock_{0};
  SteppingPoller poller_{network_, clock_};
  ScriptUdp socket_{network_};
};

TEST_F(HolepunchProbeWait, RearmsAfterEachStaleDatagram) {
  poller_.step = 40;
  socket_.scripted_only = true;
  const auto probe = profile();
  auto stale_profile = probe;
  stale_profile.request_id[0] = std::byte{9};
  const auto stale =
      nat::ProbeCodec::encode_response(stale_profile, socket_.peer);
  const auto matching = nat::ProbeCodec::encode_response(probe, socket_.peer);
  for (int i = 0; i < 3; ++i)
    socket_.replies.emplace_back(stale.begin(), stale.end());
  socket_.replies.emplace_back(matching.begin(), matching.end());
  socket_.receives = 1;
  const pf::SocketEndpoint peers[] = {socket_.peer};
  const nat::ProbeSocketGroup groups[] = {{&socket_, peers}};
  auto result = wait(groups, probe);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().peer, socket_.peer);
  EXPECT_EQ(clock_.now_us, 160U);
}

TEST_F(HolepunchProbeWait, KeepsRetryingThroughSendErrors) {
  socket_.always_would_block = true;
  socket_.fail_after = 1;
  const pf::SocketEndpoint peers[] = {socket_.peer};
  const nat::ProbeSocketGroup groups[] = {{&socket_, peers}};
  auto result = wait_with_retries(groups, nat::CandidateProbe{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
  EXPECT_EQ(socket_.sends, 4U);
}

TEST_F(HolepunchProbeWait, ReciprocatesAPeerRequestOnlyFromAReciprocalGroup) {
  for (const bool reciprocal : {false, true}) {
    SCOPED_TRACE(reciprocal);
    test_support::FakeClock clock{0};
    SteppingPoller poller{network_, clock};
    ScriptUdp socket{network_};
    socket.scripted_only = true;
    const auto probe = profile();
    auto incoming = probe;
    incoming.request_id = {std::byte{7}, std::byte{7}, std::byte{7},
                           std::byte{7}, std::byte{7}};
    const auto peer_request = nat::ProbeCodec::encode(incoming);
    socket.replies.emplace_back(peer_request.begin(), peer_request.end());
    socket.receives = 1;
    const auto request = nat::ProbeCodec::encode(probe);
    const pf::SocketEndpoint peers[] = {socket.peer};
    nat::ProbeSocketGroup group{&socket, peers};
    group.reciprocal = reciprocal;
    const nat::ProbeSocketGroup groups[] = {group};
    bool answered = false;
    auto result = nat::Probe::wait_for_response_from_groups(
        groups, poller, clock, request, probe.request_id, probe, {},
        std::chrono::microseconds{100}, &answered);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
    EXPECT_TRUE(answered);
    ASSERT_EQ(socket.sent.size(), reciprocal ? 3U : 2U);
    if (reciprocal) {
      EXPECT_TRUE(std::ranges::equal(socket.sent.back(), request));
    }
  }
}

TEST_F(HolepunchProbeWait, PlansARandomAllocationGroupAsReciprocal) {
  nat::BoundSockets bound;
  auto ipv4 = network_.sockets().create_udp_socket(pf::IpAddress::any_ipv4());
  ASSERT_TRUE(ipv4);
  bound.ipv4 = std::move(ipv4.value());
  nat::BoundSockets::add_nat_sockets(bound, network_.sockets(), 0, {});
  EXPECT_TRUE(bound.random_allocation);
  const nat::ResolvedCandidateEndpoint resolved[] = {
      {0, {pf::IpAddress::loopback_ipv4(), 4001}}};
  auto plan = nat::ProbeGroupPlan::make(bound, resolved);
  ASSERT_TRUE(plan);
  const auto groups = plan.value().groups();
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_TRUE(groups.front().reciprocal);
}

TEST_F(HolepunchProbeWait, MatchesTheReachablePeerAfterAnotherFailsToSend) {
  auto result = two_peer_wait(socket_);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().peer, socket_.peer);
  EXPECT_EQ(socket_.receives, 2U);
  EXPECT_EQ(clock_.now_us, 20U);
}

TEST_F(HolepunchProbeWait, FailsWithoutReadingWhenEverySendFails) {
  socket_.all_sends_fail = true;
  auto result = two_peer_wait(socket_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::net);
  EXPECT_EQ(socket_.receives, 0U);
}

TEST_F(HolepunchProbeWait, KeepsReadingThroughReceiveErrorsUntilTimeout) {
  socket_.receive_failure = true;
  auto result = two_peer_wait(socket_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
  EXPECT_GT(socket_.receives, 1U);
}

TEST_F(HolepunchProbeWait, TimesOutWhenNothingArrives) {
  socket_.always_would_block = true;
  auto result = two_peer_wait(socket_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
}

TEST_F(HolepunchProbeWait, SendsOnlyOnceFromANatGroup) {
  ScriptUdp base{network_};
  ScriptUdp nat_socket{network_};
  base.always_would_block = true;
  nat_socket.always_would_block = true;
  const pf::SocketEndpoint peers[] = {base.peer};
  const nat::ProbeSocketGroup groups[] = {{&base, peers},
                                          {&nat_socket, peers, true}};
  auto result = wait_with_retries(groups, nat::CandidateProbe{});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
  EXPECT_EQ(base.sends, 4U);
  EXPECT_EQ(nat_socket.sends, 1U);
}

TEST(HolepunchStunBinding, ReadsTheMappedAddressAfterAWouldBlock) {
  pf::DefaultNetworkProvider network{test_support::LiveProviders::threading(),
                                     test_support::LiveProviders::steady()};
  test_support::FakeClock clock{0};
  SteppingPoller poller{network, clock};
  ScriptUdp socket{network};
  const std::array<std::byte, 12> transaction{};
  socket.reply = {std::byte{1},    std::byte{1},    std::byte{0},
                  std::byte{12},   std::byte{0x21}, std::byte{0x12},
                  std::byte{0xa4}, std::byte{0x42}};
  socket.reply.resize(20);
  const std::byte address[]{std::byte{0},    std::byte{1},    std::byte{0},
                            std::byte{8},    std::byte{0},    std::byte{1},
                            std::byte{0x12}, std::byte{0x34}, std::byte{192},
                            std::byte{0},    std::byte{2},    std::byte{1}};
  socket.reply.insert(socket.reply.end(), std::begin(address),
                      std::end(address));
  auto mapped =
      libchiaking::net::nat::stun::BindingClient{socket, poller, clock}.query(
          socket.peer, transaction, {}, std::chrono::microseconds{100});
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped.value().port, 0x1234);
  EXPECT_EQ(socket.receives, 2U);
}

}  // namespace
}  // namespace play_test
