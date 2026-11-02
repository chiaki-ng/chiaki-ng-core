#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/holepunch_probe_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using holepunch_probe::Loopback;
using ProbePacket = std::array<std::byte, nat::CandidateProbe::kEncodedSize>;

class HolepunchProbeLoopback : public ::testing::Test {
 protected:
  void SetUp() override {
    sender_ = loopback_.socket();
    receiver_ = loopback_.socket();
    ASSERT_TRUE(sender_);
    ASSERT_TRUE(receiver_);
    sender_ep_ = Loopback::endpoint(*sender_);
    receiver_ep_ = Loopback::endpoint(*receiver_);
    probe_.request_id = {std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3},
                         std::byte{0xA4}, std::byte{0xA5}};
    matching_ = nat::ProbeCodec::encode_response(probe_, {});
    auto stale_probe = probe_;
    stale_probe.request_id = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                              std::byte{0xFF}, std::byte{0xFF}};
    stale_ = nat::ProbeCodec::encode_response(stale_probe, {});
    request_ = nat::ProbeCodec::encode(probe_);
  }

  void expect_received(nat::ProbeDatagramKind kind) {
    ASSERT_TRUE(loopback_.readable(*receiver_));
    ProbePacket buffer{};
    const auto received = receiver_->receive_from(buffer);
    ASSERT_TRUE(received);
    EXPECT_EQ(received.value().size, buffer.size());
    EXPECT_EQ(received.value().source, sender_ep_);
    EXPECT_EQ(nat::ProbeCodec::classify(buffer, probe_.request_id), kind);
  }

  auto wait(std::span<const nat::ProbeSocketGroup> groups,
            const pf::StopToken& stop,
            std::chrono::nanoseconds timeout = std::chrono::seconds{2},
            bool* answered = nullptr) {
    return nat::Probe::wait_for_response_from_groups(
        groups, loopback_.poller(), loopback_.clock(), request_,
        probe_.request_id, probe_, stop, timeout, answered);
  }

  auto wait_with_retries(std::span<const nat::ProbeSocketGroup> groups,
                         const pf::StopToken& stop,
                         nat::ProbeRetryPolicy policy) {
    return nat::Probe::wait_for_response_from_groups_with_retries(
        groups, loopback_.poller(), loopback_.clock(), request_,
        probe_.request_id, probe_, stop, policy);
  }

  auto drained_request(pf::UdpSocket& peer) -> bool {
    if (!loopback_.readable(peer)) return false;
    ProbePacket packet{};
    const auto received = peer.receive_from(packet);
    return received && received.value().size == packet.size() &&
           packet == request_;
  }

  Loopback loopback_;
  std::unique_ptr<pf::UdpSocket> sender_;
  std::unique_ptr<pf::UdpSocket> receiver_;
  pf::SocketEndpoint sender_ep_{};
  pf::SocketEndpoint receiver_ep_{};
  nat::CandidateProbe probe_{};
  ProbePacket matching_{};
  ProbePacket stale_{};
  ProbePacket request_{};
  const pf::StopToken open_;
  const pf::StopToken stopped_{std::make_shared<test_support::Stopped>()};
};

TEST_F(HolepunchProbeLoopback, BindsBothSocketsToLoopback) {
  EXPECT_TRUE(sender_ep_.address.is_loopback());
  EXPECT_TRUE(receiver_ep_.address.is_loopback());
  EXPECT_EQ(&loopback_.poller().owner(), &receiver_->owner());
}

TEST_F(HolepunchProbeLoopback, ClassifiesAMatchingResponseFromThePeer) {
  const auto sent = sender_->send_to(matching_, receiver_ep_);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), matching_.size());
  expect_received(nat::ProbeDatagramKind::matching_response);
}

TEST_F(HolepunchProbeLoopback, ClassifiesAStaleResponseFromThePeer) {
  const auto sent = sender_->send_to(stale_, receiver_ep_);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), stale_.size());
  expect_received(nat::ProbeDatagramKind::stale_response_id);
}

TEST_F(HolepunchProbeLoopback, FindsTheGroupWhoseKnownPeerMatched) {
  auto group_peer = loopback_.socket();
  auto group_waiter = loopback_.socket();
  ASSERT_TRUE(group_peer);
  ASSERT_TRUE(group_waiter);
  const auto group_peer_ep = Loopback::endpoint(*group_peer);
  const auto group_waiter_ep = Loopback::endpoint(*group_waiter);
  ASSERT_TRUE(sender_->send_to(stale_, group_waiter_ep));
  ASSERT_TRUE(group_peer->send_to(stale_, group_waiter_ep));
  ASSERT_TRUE(group_peer->send_to(matching_, group_waiter_ep));
  const pf::SocketEndpoint group0_peers[] = {sender_ep_};
  const pf::SocketEndpoint group1_peers[] = {group_peer_ep};
  const nat::ProbeSocketGroup groups[] = {
      {receiver_.get(), group0_peers},
      {group_waiter.get(), group1_peers},
  };
  const auto matched_group = wait(groups, open_);
  ASSERT_TRUE(matched_group);
  EXPECT_EQ(matched_group.value().group_index, 1U);
  EXPECT_EQ(matched_group.value().peer, group_peer_ep);
  const nat::ProbeSocketGroup duplicate_groups[] = {
      {receiver_.get(), group0_peers},
      {receiver_.get(), group0_peers},
  };
  const auto duplicate = wait(duplicate_groups, open_);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), nat::ProbeWaitError::protocol);
  const auto stopped_groups = wait(groups, stopped_);
  ASSERT_FALSE(stopped_groups);
  EXPECT_EQ(stopped_groups.error(), nat::ProbeWaitError::cancelled);
}

TEST_F(HolepunchProbeLoopback, AnswersAPeerRequestWhileWaitingForItsResponse) {
  auto req_peer = loopback_.socket();
  auto req_waiter = loopback_.socket();
  ASSERT_TRUE(req_peer);
  ASSERT_TRUE(req_waiter);
  const auto req_peer_ep = Loopback::endpoint(*req_peer);
  const auto req_waiter_ep = Loopback::endpoint(*req_waiter);
  auto groups_probe = probe_;
  groups_probe.request_id = {std::byte{0x1B}, std::byte{0x1C}, std::byte{0x1D},
                             std::byte{0x1E}, std::byte{0x1F}};
  const ProbePacket groups_request = nat::ProbeCodec::encode(groups_probe);
  ASSERT_TRUE(req_peer->send_to(groups_request, req_waiter_ep));
  ASSERT_TRUE(req_peer->send_to(matching_, req_waiter_ep));
  const pf::SocketEndpoint req_group0_peers[] = {sender_ep_};
  const pf::SocketEndpoint req_group1_peers[] = {req_peer_ep};
  const nat::ProbeSocketGroup req_groups[] = {
      {receiver_.get(), req_group0_peers},
      {req_waiter.get(), req_group1_peers},
  };
  const auto req_matched = wait(req_groups, open_);
  ASSERT_TRUE(req_matched);
  EXPECT_EQ(req_matched.value().group_index, 1U);
  EXPECT_EQ(req_matched.value().peer, req_peer_ep);
  ASSERT_TRUE(loopback_.readable(*req_peer));
  ProbePacket req_initial{};
  const auto req_got_initial = req_peer->receive_from(req_initial);
  ASSERT_TRUE(req_got_initial);
  EXPECT_EQ(req_got_initial.value().size, req_initial.size());
  EXPECT_EQ(req_got_initial.value().source, req_waiter_ep);
  EXPECT_EQ(req_initial, request_);
  ASSERT_TRUE(loopback_.readable(*req_peer));
  ProbePacket req_reply{};
  const auto req_got_reply = req_peer->receive_from(req_reply);
  ASSERT_TRUE(req_got_reply);
  EXPECT_EQ(req_got_reply.value().size, req_reply.size());
  EXPECT_EQ(req_got_reply.value().source, req_waiter_ep);
  EXPECT_EQ(req_reply,
            nat::ProbeCodec::encode_response(groups_probe, req_peer_ep));
  EXPECT_TRUE(req_waiter->close());
  EXPECT_TRUE(req_peer->close());
}

TEST_F(HolepunchProbeLoopback, ResendsToEveryGroupOnEachRetry) {
  auto peer_a = loopback_.socket();
  auto peer_b = loopback_.socket();
  auto waiter_a = loopback_.socket();
  auto waiter_b = loopback_.socket();
  ASSERT_TRUE(peer_a);
  ASSERT_TRUE(peer_b);
  ASSERT_TRUE(waiter_a);
  ASSERT_TRUE(waiter_b);
  const auto peer_a_ep = Loopback::endpoint(*peer_a);
  const auto peer_b_ep = Loopback::endpoint(*peer_b);
  const auto waiter_b_ep = Loopback::endpoint(*waiter_b);
  const pf::SocketEndpoint a_peers[] = {peer_a_ep};
  const pf::SocketEndpoint b_peers[] = {peer_b_ep};
  const nat::ProbeSocketGroup groups[] = {
      {waiter_a.get(), a_peers},
      {waiter_b.get(), b_peers},
  };
  const auto no_group_send = wait_with_retries(
      groups, stopped_, nat::ProbeRetryPolicy{2, std::chrono::nanoseconds{0}});
  ASSERT_FALSE(no_group_send);
  EXPECT_EQ(no_group_send.error(), nat::ProbeWaitError::cancelled);
  const auto three_group_sends = wait_with_retries(
      groups, open_, nat::ProbeRetryPolicy{2, std::chrono::nanoseconds{0}});
  ASSERT_FALSE(three_group_sends);
  EXPECT_EQ(three_group_sends.error(), nat::ProbeWaitError::timeout);
  for (unsigned index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(drained_request(*peer_a));
    EXPECT_TRUE(drained_request(*peer_b));
  }
  ASSERT_TRUE(peer_b->send_to(matching_, waiter_b_ep));
  const auto first_group_match = wait_with_retries(
      groups, open_, nat::ProbeRetryPolicy{2, std::chrono::seconds{2}});
  ASSERT_TRUE(first_group_match);
  EXPECT_EQ(first_group_match.value().group_index, 1U);
  EXPECT_EQ(first_group_match.value().peer, peer_b_ep);
  EXPECT_TRUE(drained_request(*peer_a));
  EXPECT_TRUE(drained_request(*peer_b));
  EXPECT_TRUE(waiter_a->close());
  EXPECT_TRUE(waiter_b->close());
  EXPECT_TRUE(peer_a->close());
  EXPECT_TRUE(peer_b->close());
}

TEST_F(HolepunchProbeLoopback, RemembersAnAnsweredRequestAcrossWaits) {
  auto waiting = loopback_.socket();
  auto console = loopback_.socket();
  ASSERT_TRUE(waiting);
  ASSERT_TRUE(console);
  const auto we = Loopback::endpoint(*waiting);
  const auto ce = Loopback::endpoint(*console);
  const pf::SocketEndpoint peers[] = {ce};
  const nat::ProbeSocketGroup reciprocal_groups[] = {{waiting.get(), peers}};
  ASSERT_TRUE(console->send_to(request_, we));
  bool answered = false;
  auto missed =
      wait(reciprocal_groups, open_, std::chrono::milliseconds{20}, &answered);
  ASSERT_FALSE(missed);
  EXPECT_EQ(missed.error(), nat::ProbeWaitError::timeout);
  EXPECT_TRUE(answered);
  ASSERT_TRUE(console->send_to(matching_, we));
  auto retried =
      wait(reciprocal_groups, open_, std::chrono::seconds{1}, &answered);
  ASSERT_TRUE(retried);
  EXPECT_TRUE(retried.value().answered_peer_request);
  auto cancelled_followup = nat::Probe::answer_candidate_requests(
      *waiting, loopback_.poller(), ce, probe_, stopped_, true);
  ASSERT_FALSE(cancelled_followup);
  EXPECT_EQ(cancelled_followup.error(), nat::ProbeWaitError::cancelled);
}

TEST_F(HolepunchProbeLoopback, ClosesASocketOnlyOnce) {
  EXPECT_TRUE(receiver_->close());
  EXPECT_FALSE(receiver_->close().has_value());
  EXPECT_TRUE(sender_->close());
  EXPECT_FALSE(sender_->close().has_value());
}

TEST_F(HolepunchProbeLoopback, RetriesIncludeDiscoveredPeers) {
  auto waiter = loopback_.socket();
  auto known = loopback_.socket();
  auto derived = loopback_.socket();
  ASSERT_TRUE(waiter);
  ASSERT_TRUE(known);
  ASSERT_TRUE(derived);
  const auto waiter_ep = Loopback::endpoint(*waiter);
  const auto known_ep = Loopback::endpoint(*known);
  nat::CandidateProbe probe{};
  probe.request_id = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
                      std::byte{0x44}, std::byte{0x55}};
  const ProbePacket request = nat::ProbeCodec::encode(probe);
  auto peer_probe = probe;
  peer_probe.request_id = {std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
                           std::byte{0x99}, std::byte{0xAA}};
  const ProbePacket peer_request = nat::ProbeCodec::encode(peer_probe);
  ASSERT_TRUE(derived->send_to(peer_request, waiter_ep));
  const pf::SocketEndpoint peers[] = {known_ep};
  const nat::ProbeSocketGroup groups[] = {{waiter.get(), peers}};
  const auto result = nat::Probe::wait_for_response_from_groups_with_retries(
      groups, loopback_.poller(), loopback_.clock(), request, probe.request_id,
      probe, open_, nat::ProbeRetryPolicy{2, std::chrono::milliseconds{50}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeWaitError::timeout);
  const auto count = [&](pf::UdpSocket& socket, unsigned& requests,
                         unsigned& others) {
    while (loopback_.readable(socket, std::chrono::milliseconds{100})) {
      ProbePacket packet{};
      if (!socket.receive_from(packet)) return;
      if (packet == request)
        ++requests;
      else
        ++others;
    }
  };
  unsigned known_requests = 0;
  unsigned known_other = 0;
  count(*known, known_requests, known_other);
  unsigned derived_requests = 0;
  unsigned derived_responses = 0;
  count(*derived, derived_requests, derived_responses);
  EXPECT_EQ(known_requests, 3U);
  EXPECT_EQ(known_other, 0U);
  EXPECT_EQ(derived_responses, 1U);
  EXPECT_EQ(derived_requests, 3U);
}

}  // namespace
}  // namespace play_test
