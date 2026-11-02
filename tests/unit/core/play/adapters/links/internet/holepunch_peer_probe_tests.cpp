#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/holepunch_probe_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace pf = libchiaking::platform;
using holepunch_probe::Loopback;

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
    if (fail) return libchiaking::unexpected(pf::PlatformError::io_failure);
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::byte>(0xA0U + i);
    return {};
  }

  int calls{};
  bool fail{};
};

class HolepunchPeerProbe : public ::testing::Test {
 protected:
  void SetUp() override {
    peer_ = loopback_.socket();
    ASSERT_TRUE(peer_);
    peer_ep_ = Loopback::endpoint(*peer_);
    profile_.request_id = {std::byte{0xA0}, std::byte{0xA1}, std::byte{0xA2},
                           std::byte{0xA3}, std::byte{0xA4}};
    console_probe_ = profile_;
    console_probe_.request_id[0] = std::byte{0x55};
  }

  static auto candidate(std::string_view address, std::uint16_t port)
      -> nat::Candidate {
    nat::Candidate result{};
    result.addr = address;
    result.port = port;
    return result;
  }

  auto input(std::uint16_t port) const -> nat::PeerProbeInput {
    nat::PeerProbeInput input{};
    input.candidates = {candidate("dead", 1000), candidate("peer", port)};
    input.probe.local_sid = 1;
    input.probe.console_sid = 2;
    input.retry = {0, std::chrono::seconds{2}};
    return input;
  }

  auto probe(const nat::PeerProbeInput& input, const pf::StopToken& stop = {}) {
    return nat::Probe::probe_peer(loopback_.bound_sockets(), dns_,
                                  loopback_.poller(), loopback_.clock(),
                                  random_, stop, input);
  }

  void win(bool request_first, bool derived) {
    auto sockets = loopback_.bound_sockets();
    ASSERT_TRUE(sockets.ipv4);
    auto* owned = sockets.ipv4.get();
    const auto response = nat::ProbeCodec::encode_response(profile_, {});
    const pf::SocketEndpoint to{pf::IpAddress::loopback_ipv4(),
                                sockets.local_port};
    const auto request = nat::ProbeCodec::encode(console_probe_);
    if (request_first) {
      ASSERT_TRUE(peer_->send_to(request, to));
    }
    const auto sent = peer_->send_to(response, to);
    ASSERT_TRUE(sent);
    ASSERT_EQ(sent.value(), response.size());
    if (!request_first) {
      ASSERT_TRUE(peer_->send_to(request, to));
    }
    input_ = input(derived ? std::uint16_t{9} : peer_ep_.port);
    auto winner =
        nat::Probe::probe_peer(std::move(sockets), dns_, loopback_.poller(),
                               loopback_.clock(), random_, open_, input_);
    ASSERT_TRUE(winner);
    EXPECT_EQ(winner.value().socket.get(), owned);
    EXPECT_EQ(winner.value().candidate_index,
              derived ? std::numeric_limits<std::size_t>::max() : 1U);
    EXPECT_EQ(winner.value().derived, derived);
    EXPECT_EQ(winner.value().peer, peer_ep_);
    EXPECT_EQ(dns_.calls, 2);
    EXPECT_EQ(random_.calls, 1);
    winner_ = std::move(winner.value());
  }

  void expect_answered() {
    std::array<std::byte, 88> actual{};
    auto first = peer_->receive_from(actual);
    ASSERT_TRUE(first);
    EXPECT_TRUE(nat::ProbeCodec::decode(actual));
    auto second = peer_->receive_from(actual);
    auto expected_profile = console_probe_;
    expected_profile.local_sid = 1;
    expected_profile.console_sid = 2;
    const auto expected = nat::ProbeCodec::encode_response(
        expected_profile, {pf::IpAddress::loopback_ipv4(), peer_ep_.port});
    ASSERT_TRUE(second);
    EXPECT_EQ(actual, expected);
    auto unanswered = nat::Probe::answer_candidate_requests(
        *winner_->socket, loopback_.poller(), peer_ep_, expected_profile, open_,
        false);
    ASSERT_FALSE(unanswered);
    EXPECT_EQ(unanswered.error(), nat::ProbeWaitError::timeout);
  }

  Loopback loopback_;
  PeerOnlyDns dns_;
  RampRandom random_;
  const pf::StopToken open_;
  std::unique_ptr<pf::UdpSocket> peer_;
  pf::SocketEndpoint peer_ep_{};
  nat::CandidateProbe profile_{};
  nat::CandidateProbe console_probe_{};
  nat::PeerProbeInput input_{};
  std::optional<nat::SelectedCandidate> winner_;
};

TEST_F(HolepunchPeerProbe, AnswersTheConsolesProbeThatFollowsItsResponse) {
  ASSERT_NO_FATAL_FAILURE(win(false, false));
  expect_answered();
}

TEST_F(HolepunchPeerProbe, AnswersTheConsolesProbeThatPrecedesItsResponse) {
  ASSERT_NO_FATAL_FAILURE(win(true, false));
  expect_answered();
}

TEST_F(HolepunchPeerProbe, OffersADerivedCandidateForAnUnofferedPort) {
  ASSERT_NO_FATAL_FAILURE(win(true, true));
  internet::holepunch::PreparedOffer ours{};
  ours.local = {nat::CandidateType::local, "192.0.2.2", "0.0.0.0", 1000, 0};
  ours.request.sid = 1;
  ours.request.candidates = {
      {nat::CandidateType::static_, "203.0.113.2", "0.0.0.0", 2000, 0},
      ours.local};
  nat::ConnRequest console{};
  console.sid = 2;
  console.candidates = input_.candidates;
  auto accept =
      internet::holepunch::Accept::build_selected(ours, console, *winner_);
  ASSERT_TRUE(accept);
  ASSERT_EQ(accept.value().candidates.size(), 1U);
  const auto& selected = accept.value().candidates.front();
  EXPECT_EQ(selected.type, nat::CandidateType::derived);
  EXPECT_EQ(selected.addr, "127.0.0.1");
  EXPECT_EQ(selected.port, peer_ep_.port);
  EXPECT_EQ(selected.mapped_addr, "203.0.113.2");
  EXPECT_EQ(selected.mapped_port, 2000);
  std::array<std::byte, 88> reply{};
  auto got = peer_->receive_from(reply);
  ASSERT_TRUE(got);
  EXPECT_EQ(nat::ProbeCodec::classify(reply, console_probe_.request_id),
            nat::ProbeDatagramKind::matching_response);
  got = peer_->receive_from(reply);
  ASSERT_TRUE(got);
  EXPECT_EQ(reply, nat::ProbeCodec::encode(
                       nat::CandidateProbe{.local_hashed_id = {},
                                           .console_hashed_id = {},
                                           .local_sid = 1,
                                           .console_sid = 2,
                                           .request_id = profile_.request_id}));
}

TEST_F(HolepunchPeerProbe, RefusesAPeerWithoutCandidates) {
  auto none = probe(nat::PeerProbeInput{});
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), nat::PeerProbeError::no_candidates);
}

TEST_F(HolepunchPeerProbe, RefusesASessionIdBeyondSixteenBitsBeforeDrawing) {
  auto bad = input(peer_ep_.port);
  bad.probe.local_sid = 65536;
  auto invalid = probe(bad);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), nat::PeerProbeError::invalid_sid);
  EXPECT_EQ(random_.calls, 0);
}

TEST_F(HolepunchPeerProbe, StopsBeforeResolvingWhenCancelled) {
  pf::StopToken stopped{std::make_shared<test_support::Stopped>()};
  auto cancelled = probe(input(peer_ep_.port), stopped);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), nat::PeerProbeError::cancelled);
  EXPECT_EQ(dns_.calls, 0);
  EXPECT_EQ(random_.calls, 0);
}

TEST_F(HolepunchPeerProbe, TimesOutWhenNoCandidateAnswers) {
  auto timeout_input = input(peer_ep_.port);
  timeout_input.retry = {0, std::chrono::nanoseconds{0}};
  auto timed_out = probe(timeout_input);
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), nat::PeerProbeError::wait_timeout);
}

TEST_F(HolepunchPeerProbe, PassesOnARandomFailure) {
  random_.fail = true;
  auto random_failed = probe(input(peer_ep_.port));
  ASSERT_FALSE(random_failed);
  EXPECT_EQ(random_failed.error(), nat::PeerProbeError::random_failed);
}

class HolepunchProbeDerivedPeers : public ::testing::Test {
 protected:
  auto wait(nat::ProbeSocketGroup& group, const nat::CandidateProbe& profile,
            std::span<const std::byte> packet) {
    return nat::Probe::wait_for_response_from_groups(
        std::span{&group, 1}, loopback_.poller(), loopback_.clock(), packet,
        profile.request_id, profile, {}, std::chrono::milliseconds{10},
        &answered_, &discovered_);
  }

  Loopback loopback_;
  nat::ProbeDiscoveryState discovered_;
  bool answered_ = false;
};

TEST_F(HolepunchProbeDerivedPeers, AcceptsAtMostThreeDerivedPeers) {
  auto receiver = loopback_.bound_sockets();
  ASSERT_TRUE(receiver.ipv4);
  std::vector<std::unique_ptr<pf::UdpSocket>> senders;
  std::vector<pf::SocketEndpoint> endpoints;
  for (int i = 0; i < 5; ++i) {
    auto socket = loopback_.socket();
    ASSERT_TRUE(socket);
    endpoints.push_back(Loopback::endpoint(*socket));
    senders.push_back(std::move(socket));
  }
  nat::CandidateProbe profile{};
  profile.request_id[0] = std::byte{42};
  const auto packet = nat::ProbeCodec::encode(profile);
  const auto response = nat::ProbeCodec::encode_response(profile, {});
  const pf::SocketEndpoint to{pf::IpAddress::loopback_ipv4(),
                              receiver.local_port};
  nat::ProbeSocketGroup group{receiver.ipv4.get(),
                              std::span{endpoints.data(), 1}};
  for (std::size_t i = 1; i < endpoints.size(); ++i) {
    SCOPED_TRACE(i);
    ASSERT_TRUE(senders[i]->send_to(response, to));
    auto result = wait(group, profile, packet);
    if (i <= 3) {
      ASSERT_TRUE(result);
      EXPECT_TRUE(result.value().derived);
      EXPECT_EQ(result.value().peer, endpoints[i]);
    } else {
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
    }
  }
  EXPECT_EQ(discovered_.derived.size(), 3U);
  ASSERT_TRUE(senders[1]->send_to(response, to));
  auto duplicate = wait(group, profile, packet);
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate.value().peer, endpoints[1]);
  EXPECT_EQ(discovered_.derived.size(), 3U);
}

}  // namespace
}  // namespace play_test
