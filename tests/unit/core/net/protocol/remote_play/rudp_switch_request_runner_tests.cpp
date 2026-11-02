#include "tests/unit/core/net/protocol/remote_play/rudp_switch_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace tsd = remote_play::transport_switch_detail;
using remote_play::RudpSwitchBarrierError;

class RudpSwitchRequestRunnerTest : public RudpSwitchRig {
 protected:
  void SetUp() override {
    RudpSwitchRig::SetUp();
    channel.clock = &clock;
  }

  auto run(const remote_play::RudpSwitchHooks& hooks = {},
           const platform::StopToken& stop = {}) {
    return remote_play::RudpSwitchRequestRunner::run(
        channel, state, input, {random, clock, stop, scratch}, hooks);
  }

  static auto ack(std::uint16_t counter, std::uint16_t acknowledged)
      -> libchiaking::net::transport::datagram::OwnedDatagram {
    return rudp::FrameCodec::encode_ack_message(counter, acknowledged).value();
  }

  static auto is_heartbeat(const remote_play::RudpCtrlSwitchStepResult& message)
      -> bool {
    return std::holds_alternative<remote_play::CtrlHeartbeatRequest>(
        message.message.message);
  }

  test_support::FixedRandom random;
  test_support::FakeClock clock{0};
  RudpSwitchNoCrypto crypto;
  remote_play::AuthMaterial material;
  remote_play::RudpCtrlByteStream stream;
  remote_play::RudpCtrlSwitchStepInput input{
      crypto, remote_play::RegistrationTarget::ps5_1, material, 0, 0x1234, 7,
      stream};
  RudpSwitchChannel channel;
  tsd::RemoteTransportSwitch state;
  rudp::SendBuffer scratch;
};

TEST_F(RudpSwitchRequestRunnerTest, ResendsTheSameRequestUntilItTimesOut) {
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::timeout);
  ASSERT_EQ(channel.sent.size(), 9U);
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(channel.sent[0], channel.sent[1]);
  EXPECT_EQ(channel.sent[1], channel.sent[2]);
  std::vector<std::byte> request{
      std::byte{0xC0}, std::byte{0x22}, std::byte{0x24}, std::byte{0x4F},
      std::byte{0x24}, std::byte{0x4F}, std::byte{0x02}, std::byte{0x30},
      std::byte{0x12}, std::byte{0x34}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x10}, std::byte{0x00}, std::byte{0x0D},
      std::byte{0x00}, std::byte{0x00}};
  request.resize(34, std::byte{0x42});
  EXPECT_EQ(channel.sent[0], request);
  EXPECT_EQ(clock.now_us, 5'000'000U);
  EXPECT_EQ(state.state(), tsd::State::timed_out);
}

TEST_F(RudpSwitchRequestRunnerTest, ForwardsControlThatArrivesBeforeTheSwitch) {
  unsigned received = 0;
  channel.on_receive = [&] {
    channel.inbox = received++ < 2 ? ack(0x5677, 0x1234) : switch_packet;
  };
  unsigned forwarded = 0;
  const auto result = run(
      {[&](remote_play::RudpCtrlSwitchStepResult&& message) {
         ++forwarded;
         EXPECT_EQ(message.delivery, remote_play::RudpCtrlDelivery::control);
         return true;
       },
       {}});
  ASSERT_TRUE(result);
  EXPECT_EQ(received, 3U);
  EXPECT_EQ(forwarded, 1U);
  EXPECT_EQ(channel.sent.size(), 2U);
  EXPECT_EQ(result.value().next_remote_crypt_counter,
            input.remote_crypt_counter);
}

TEST_F(RudpSwitchRequestRunnerTest, ForwardsAHeartbeatPackedAfterTheSwitch) {
  auto combined = switch_packet;
  const auto heartbeat = heartbeat_packet();
  combined.insert(combined.end(), heartbeat.begin(), heartbeat.end());
  channel.inbox = combined;
  unsigned forwarded = 0;
  const auto result =
      run({[&](remote_play::RudpCtrlSwitchStepResult&& message) {
             if (forwarded++ == 0)
               EXPECT_TRUE(message.switch_notification);
             else
               EXPECT_TRUE(is_heartbeat(message));
             return true;
           },
           {}});
  ASSERT_TRUE(result);
  EXPECT_EQ(forwarded, 2U);
  EXPECT_EQ(channel.sent.size(), 3U);
  EXPECT_EQ(state.state(), tsd::State::ready);
}

TEST_F(RudpSwitchRequestRunnerTest, AcknowledgesTheSwitchAndBecomesReady) {
  channel.inbox = switch_packet;
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().attempt_id, 7U);
  EXPECT_EQ(result.value().next_local_counter, 0x1235);
  ASSERT_EQ(channel.sent.size(), 2U);
  EXPECT_EQ(channel.sent[1], ack(0x1235, 0x5678));
  EXPECT_EQ(state.state(), tsd::State::ready);
  EXPECT_TRUE(state.take_ready());
}

TEST_F(RudpSwitchRequestRunnerTest, WrapsTheLocalCounter) {
  channel.inbox = switch_packet;
  input.local_counter = 0xffff;
  int forwarded = 0;
  const auto result =
      run({[&](remote_play::RudpCtrlSwitchStepResult&& message) {
             ++forwarded;
             return message.switch_notification && message.attempt_id == 7;
           },
           {}});
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().next_local_counter, 0);
  EXPECT_EQ(forwarded, 1);
}

TEST_F(RudpSwitchRequestRunnerTest, ARefusedMessageCancelsTheAttempt) {
  channel.inbox = switch_packet;
  int forwarded = 0;
  const auto result = run({[&](remote_play::RudpCtrlSwitchStepResult&&) {
                             ++forwarded;
                             return false;
                           },
                           {}});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::invalid_attempt);
  EXPECT_EQ(forwarded, 1);
  EXPECT_EQ(state.state(), tsd::State::cancelled);
}

TEST_F(RudpSwitchRequestRunnerTest, AnswersAHeartbeatBeforeTheSwitchArrives) {
  unsigned receives = 0;
  bool heartbeat_pending = false;
  channel.on_receive = [&] {
    if (receives++ == 0) {
      channel.inbox = heartbeat_packet();
      return;
    }
    const bool replied =
        std::ranges::any_of(channel.sent, [](const auto& bytes) {
          return bytes.size() >= 18 && bytes[14] == std::byte{1} &&
                 bytes[15] == std::byte{0xfe};
        });
    if (replied) {
      auto packet = ack(0x5677, 0x1236);
      const auto notification = switch_packet;
      packet.insert(packet.end(), notification.begin(), notification.end());
      channel.inbox = std::move(packet);
    }
  };
  const auto result =
      run({[&](remote_play::RudpCtrlSwitchStepResult&& message) {
             heartbeat_pending = is_heartbeat(message);
             return true;
           },
           [&](rudp::HandshakeChannel& reliable,
               std::uint16_t counter) -> std::optional<std::uint16_t> {
             if (!heartbeat_pending) return counter;
             auto packet = remote_play::RudpCtrl::encode_outbound(
                 crypto, input.target, material, 0x01fe, {}, 0, counter);
             EXPECT_TRUE(packet);
             if (!packet) return std::nullopt;
             EXPECT_TRUE(reliable.send(packet.value().bytes));
             heartbeat_pending = false;
             return packet.value().next_rudp_counter;
           }});
  ASSERT_TRUE(result);
  EXPECT_EQ(receives, 2U);
  EXPECT_EQ(clock.now_us, 0U);
  EXPECT_EQ(result.value().next_local_counter, 0x1236);
  EXPECT_EQ(scratch.size(), 0U);
  EXPECT_EQ(channel.sent.back(), ack(0x1236, 0x5678));
}

TEST_F(RudpSwitchRequestRunnerTest, AcceptsASwitchJustBeforeTheDeadline) {
  channel.on_receive = [&] {
    if (clock.now_us >= 4800000) channel.inbox = switch_packet;
  };
  const auto result = run();
  EXPECT_TRUE(result);
  EXPECT_EQ(clock.now_us, 4800000U);
}

TEST_F(RudpSwitchRequestRunnerTest, AnAckAloneDoesNotCompleteTheSwitch) {
  channel.inbox = ack(0x5677, 0x1235);
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::timeout);
  EXPECT_EQ(channel.sent.size(), 1U);
  EXPECT_EQ(clock.now_us, 5000000U);
}

TEST_F(RudpSwitchRequestRunnerTest, AMalformedPacketEndsTheWait) {
  channel.inbox = std::vector<std::byte>{std::byte{0xff}};
  bool replied = false;
  channel.on_receive = [&] {
    if (!channel.inbox && !replied) {
      replied = true;
      channel.inbox = switch_packet;
    }
  };
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::malformed);
  EXPECT_FALSE(replied);
}

TEST_F(RudpSwitchRequestRunnerTest, StopsWhenStoppedDuringTheWait) {
  channel.inbox = switch_packet;
  auto stopped = std::make_shared<test_support::StopState>();
  const auto result =
      run({[&](remote_play::RudpCtrlSwitchStepResult&&) {
             stopped->stopped = true;
             return true;
           },
           [](rudp::HandshakeChannel&,
              std::uint16_t) -> std::optional<std::uint16_t> {
             ADD_FAILURE() << "the control channel was serviced after a stop";
             return std::nullopt;
           }},
          platform::StopToken{stopped});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::stopped);
}

TEST_F(RudpSwitchRequestRunnerTest, AnEarlyEmptyReceiveKeepsWaiting) {
  channel.early_empty = true;
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::timeout);
  EXPECT_EQ(clock.now_us, 5000000U);
}

TEST_F(RudpSwitchRequestRunnerTest, StopsBeforeSendingWhenAlreadyStopped) {
  auto stopped = std::make_shared<test_support::StopState>();
  stopped->stopped = true;
  const auto result = run({}, platform::StopToken{stopped});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), RudpSwitchBarrierError::stopped);
  EXPECT_TRUE(channel.sent.empty());
  EXPECT_EQ(state.state(), tsd::State::idle);
}

struct RudpSwitchChannelFailure {
  std::string name;
  bool fail_send;
  bool short_send;
  bool fail_receive;
  bool cancel_on_receive;
  RudpSwitchBarrierError expected;
};

class RudpSwitchRequestRunnerFailure
    : public RudpSwitchRequestRunnerTest,
      public ::testing::WithParamInterface<RudpSwitchChannelFailure> {};

TEST_P(RudpSwitchRequestRunnerFailure, CancelsTheAttempt) {
  const auto& failure = GetParam();
  channel.fail_send = failure.fail_send;
  channel.short_send = failure.short_send;
  channel.fail_receive = failure.fail_receive;
  if (failure.cancel_on_receive) {
    channel.inbox = switch_packet;
    channel.on_receive = [this] { EXPECT_TRUE(state.cancel(7)); };
  }
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), failure.expected);
  EXPECT_EQ(state.state(), tsd::State::cancelled);
  EXPECT_LE(channel.sent.size(), 2U);
}

INSTANTIATE_TEST_SUITE_P(
    Channels, RudpSwitchRequestRunnerFailure,
    ::testing::Values(
        RudpSwitchChannelFailure{"FailedSend", true, false, false, false,
                                 RudpSwitchBarrierError::send_failed},
        RudpSwitchChannelFailure{"ShortSend", false, true, false, false,
                                 RudpSwitchBarrierError::short_send},
        RudpSwitchChannelFailure{"FailedReceive", false, false, true, false,
                                 RudpSwitchBarrierError::receive_failed},
        RudpSwitchChannelFailure{"CancelledWhileWaiting", false, false, false,
                                 true,
                                 RudpSwitchBarrierError::invalid_attempt}),
    [](const auto& tested) { return tested.param.name; });

class RudpDataSwitchTest : public RudpSwitchRequestRunnerTest {
 protected:
  std::shared_ptr<RudpSwitchRouteState> route_state =
      std::make_shared<RudpSwitchRouteState>();
};

TEST_F(RudpDataSwitchTest, HandsBackTheRouteOnceTheConsoleSwitches) {
  channel.inbox = switch_packet;
  auto switched = remote_play::RudpDataSwitch::switch_route(
      route(route_state), channel, input, {random, clock, {}, scratch});
  ASSERT_TRUE(switched);
  ASSERT_TRUE(switched.value().route.transport);
  EXPECT_EQ(switched.value().route.calibration.takion_version, 7U);
  EXPECT_EQ(route_state->close_count, 0U);
  EXPECT_TRUE(switched.value().route.transport.close());
  EXPECT_EQ(route_state->close_count, 1U);
}

TEST_F(RudpDataSwitchTest, ClosesTheRouteWhenTheSwitchTimesOut) {
  const auto failed = remote_play::RudpDataSwitch::switch_route(
      route(route_state), channel, input, {random, clock, {}, scratch});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), RudpSwitchBarrierError::timeout);
  EXPECT_EQ(route_state->close_count, 1U);
}

}  // namespace
}  // namespace remote_play_test
