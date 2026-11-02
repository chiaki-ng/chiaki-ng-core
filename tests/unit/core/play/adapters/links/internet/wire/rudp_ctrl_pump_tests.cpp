#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using rudp_ctrl::CounterCrypto;
using rudp_ctrl::SwitchChannel;
using PumpError = internet::remote_ctrl::PumpError;

class RemoteCtrlPump : public ::testing::Test {
 protected:
  auto pump(internet::remote_ctrl::PumpState state)
      -> internet::remote_ctrl::Pump {
    return internet::remote_ctrl::Pump(channel_, inbox_, crypto_,
                                       remote_play::RegistrationTarget::ps5_1,
                                       material_, std::move(state));
  }

  auto started_pump(internet::remote_ctrl::PumpState state)
      -> internet::remote_ctrl::Pump {
    EXPECT_TRUE(inbox_.start());
    return pump(std::move(state));
  }

  static auto pump_once(internet::remote_ctrl::Pump& pump,
                        std::uint64_t attempt = 7) {
    return pump.pump_once(attempt, std::chrono::microseconds{1}, {});
  }

  static auto ack_of(std::uint16_t local, std::uint16_t remote)
      -> std::vector<std::byte> {
    auto ack = rudp::FrameCodec::encode_ack_message(local, remote);
    EXPECT_TRUE(ack);
    if (!ack) return {};
    return std::move(ack.value());
  }

  SwitchChannel channel_;
  internet::remote_ctrl::Inbox inbox_{libchiaking::SecretArray<16>{}, 7};

 private:
  CounterCrypto crypto_;
  remote_play::AuthMaterial material_;
};

TEST_F(RemoteCtrlPump, IgnoresRepeatedAcknowledgementsOfTheSessionRequest) {
  auto pump = started_pump({42, 0x1234, 0x3456, 7});
  for (unsigned duplicate = 0; duplicate < 2; ++duplicate) {
    SCOPED_TRACE(duplicate);
    const auto ack = ack_of(0x5677, 0x1234);
    EXPECT_EQ(ack.size(), 14U);
    channel_.deliver(ack);
    EXPECT_TRUE(pump_once(pump));
    EXPECT_TRUE(channel_.sent().empty());
    EXPECT_FALSE(inbox_.front_effect());
    EXPECT_EQ(pump.state().remote_crypt_counter, 42U);
    EXPECT_EQ(pump.state().local_counter, 0x1234);
    EXPECT_EQ(pump.state().remote_counter, 0x3456);
  }
  channel_.deliver(SwitchChannel::switch_packet());
  EXPECT_TRUE(pump_once(pump));
  EXPECT_EQ(pump.state().remote_crypt_counter, 42U);
  EXPECT_EQ(pump.state().remote_counter, 0x5678);
  EXPECT_TRUE(inbox_.front_effect());
}

TEST_F(RemoteCtrlPump, ReportsAMalformedPacketAndWhatItSaw) {
  channel_.deliver(std::vector<std::byte>(8, std::byte{0}));
  auto pump = started_pump({42, 0x1234, 0x3456, 7});
  const auto result = pump_once(pump);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PumpError::malformed);
  EXPECT_EQ(pump.state().remote_crypt_counter, 42U);
  EXPECT_FALSE(inbox_.front_effect());
  EXPECT_EQ(pump.state().last_receive.packet_bytes, 8U);
  EXPECT_EQ(pump.state().last_receive.framing_error,
            rudp::FrameError::malformed);
}

TEST_F(RemoteCtrlPump, ReportsThePeerClosingAndDescribesIt) {
  const rudp::Frame finish{0xc000, std::vector<std::byte>(4)};
  auto encoded = rudp::FrameCodec::encode({&finish, 1});
  ASSERT_TRUE(encoded);
  channel_.deliver(std::move(encoded.value()));
  auto pump = started_pump({42, 0x1234, 0x3456, 7});
  const auto result = pump_once(pump);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PumpError::peer_closed);
  EXPECT_EQ(pump.state().remote_crypt_counter, 42U);
  EXPECT_FALSE(inbox_.front_effect());
  const play::LinkFailure detail{
      remote_play::FailureStage::ctrl_receive,
      remote_play::StreamError::ctrl_failed,
      play::LinkFailure::RemoteCtrlRead{result.error()}};
  EXPECT_NE(detail.describe(), "the RUDP Ctrl receive step failed");
}

TEST_F(RemoteCtrlPump, KeepsTheMessageWhenTheAckSendFails) {
  channel_.deliver(SwitchChannel::switch_packet());
  channel_.fail_send(true);
  auto pump = started_pump({42, 0x1234, 0x3456, 7});
  EXPECT_TRUE(pump_once(pump));
  EXPECT_TRUE(inbox_.front_effect());
  EXPECT_EQ(pump.state().last_receive.ack_send_failures, 1U);
}

TEST_F(RemoteCtrlPump, KeepsTheMessageWhenTheAckSendIsShort) {
  channel_.deliver(SwitchChannel::switch_packet());
  channel_.short_send(true);
  auto pump = started_pump({42, 0x1234, 0x3456, 7});
  EXPECT_TRUE(pump_once(pump));
  EXPECT_TRUE(inbox_.front_effect());
  EXPECT_EQ(pump.state().last_receive.ack_send_failures, 1U);
}

TEST_F(RemoteCtrlPump, AcknowledgesAndPublishesAStreamSwitchAck) {
  channel_.deliver(SwitchChannel::switch_packet());
  auto pump = started_pump({0, 0x1234, 0, 7});
  EXPECT_TRUE(pump_once(pump));
  EXPECT_EQ(channel_.sent(), ack_of(0x1234, 0x5678));
  EXPECT_EQ(pump.state().local_counter, 0x1234);
  EXPECT_EQ(pump.state().remote_counter, 0x5678);
  EXPECT_EQ(pump.state().remote_crypt_counter, 0U);
  const auto effect = inbox_.take_effect();
  ASSERT_TRUE(effect);
  EXPECT_TRUE(std::holds_alternative<lifecycle::PublishSwitchAck>(*effect));
}

TEST_F(RemoteCtrlPump, RefusesAnotherAttemptAndThenTimesOutWhenIdle) {
  channel_.deliver(SwitchChannel::switch_packet());
  auto pump = started_pump({0, 0x1234, 0, 7});
  ASSERT_TRUE(pump_once(pump));
  ASSERT_TRUE(inbox_.take_effect());
  const auto before = pump.state();
  EXPECT_FALSE(pump_once(pump, 6));
  EXPECT_EQ(pump.state().local_counter, before.local_counter);
  EXPECT_EQ(pump.state().remote_counter, before.remote_counter);
  EXPECT_FALSE(inbox_.front_effect());
  const auto received = pump_once(pump);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), PumpError::timeout);
  EXPECT_EQ(pump.state().local_counter, before.local_counter);
  EXPECT_EQ(pump.state().remote_counter, before.remote_counter);
}

TEST_F(RemoteCtrlPump, ReportsAFailedReceiveAndKeepsItsCounters) {
  channel_.fail_receive(true);
  auto pump = started_pump({0, 0x1234, 0x5678, 7});
  const auto received = pump_once(pump);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), PumpError::receive_failed);
  EXPECT_EQ(pump.state().remote_counter, 0x5678);
}

TEST_F(RemoteCtrlPump, AcknowledgesAMessageAnIdleInboxRejects) {
  channel_.deliver(SwitchChannel::switch_packet());
  auto idle = pump({0, 0x1234, 0, 7});
  const auto received = pump_once(idle);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), PumpError::inbox_rejected);
  EXPECT_EQ(channel_.sent(), ack_of(0x1234, 0x5678));
  EXPECT_EQ(idle.state().remote_counter, 0x5678);
  EXPECT_EQ(idle.state().local_counter, 0x1234);
}

}  // namespace
}  // namespace play_test
