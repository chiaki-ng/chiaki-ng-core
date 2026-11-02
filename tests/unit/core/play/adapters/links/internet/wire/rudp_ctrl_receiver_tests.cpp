#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace platform = libchiaking::platform;
namespace transport_switch = remote_play::transport_switch_detail;
using rudp_ctrl::CounterCrypto;
using rudp_ctrl::SwitchChannel;
using StepError = remote_play::RudpCtrlSwitchStepError;

class RudpCtrlReceiver : public ::testing::Test {
 protected:
  static constexpr auto target = remote_play::RegistrationTarget::ps5_1;
  static constexpr std::uint16_t local_counter = 0x1234;
  static constexpr std::uint64_t attempt_id = 7;

  auto step_on(SwitchChannel& channel) {
    return remote_play::RudpCtrlReceiver{}.step(
        channel,
        {crypto_, target, material_, 0, local_counter, attempt_id, stream_},
        std::chrono::microseconds{1}, {});
  }

  auto step() { return step_on(channel_); }

  auto step_counting(std::uint64_t remote_crypt_counter,
                     platform::StopToken stop = {}) {
    return remote_play::RudpCtrlReceiver{}.step(
        channel_,
        {counting_, target, material_, remote_crypt_counter, local_counter,
         attempt_id, stream_},
        std::chrono::microseconds{1}, stop);
  }

  void deliver_enveloped_messages() {
    const std::array frames{
        rudp::Frame{
            0x1230,
            {std::byte{0x56}, std::byte{0x77}, std::byte{}, std::byte{}}},
        message_frame(0x5678), rudp::Frame{0x2430, std::vector<std::byte>(6)},
        message_frame(0x5679)};
    auto encoded = rudp::FrameCodec::encode(frames);
    ASSERT_TRUE(encoded);
    channel_.deliver(std::move(encoded.value()));
  }

  static auto message_frame(std::uint16_t counter) -> rudp::Frame {
    auto header = remote_play::CtrlCodec::encode_frame_header(0x7777, 1);
    EXPECT_TRUE(header);
    std::vector<std::byte> bytes{static_cast<std::byte>(counter >> 8U),
                                 static_cast<std::byte>(counter)};
    if (header) bytes.insert(bytes.end(), header->begin(), header->end());
    bytes.push_back(std::byte{0x55});
    return rudp::Frame{0x0230, std::move(bytes)};
  }

  static auto ack_of(std::uint16_t remote) -> std::vector<std::byte> {
    auto ack = rudp::FrameCodec::encode_ack_message(local_counter, remote);
    EXPECT_TRUE(ack);
    if (!ack) return {};
    return std::move(ack.value());
  }

  SwitchChannel channel_;
  CounterCrypto counting_{true};

 private:
  CounterCrypto crypto_;
  remote_play::AuthMaterial material_;
  remote_play::RudpCtrlByteStream stream_;
};

TEST_F(RudpCtrlReceiver, AcknowledgesEnvelopesWithoutDecryptingThem) {
  deliver_enveloped_messages();
  const auto result = step_counting(42);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 4U);
  EXPECT_EQ(result.value()[0].delivery,
            remote_play::RudpCtrlDelivery::transport);
  EXPECT_EQ(result.value()[2].delivery,
            remote_play::RudpCtrlDelivery::acknowledgement);
  EXPECT_EQ(result.value()[1].message.next_remote_crypt_counter, 43U);
  EXPECT_EQ(result.value()[3].message.next_remote_crypt_counter, 44U);
  EXPECT_EQ(counting_.cipher_counters(), (std::vector<std::uint64_t>{42, 43}));
  EXPECT_EQ(channel_.sent_packets().size(), 3U);
  for (const auto& sent : channel_.sent_packets())
    EXPECT_EQ(sent, ack_of(0x5678));
}

TEST_F(RudpCtrlReceiver, RefusesAnExhaustedCounter) {
  deliver_enveloped_messages();
  const auto exhausted =
      step_counting(std::numeric_limits<std::uint64_t>::max());
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error(), StepError::counter_exhausted);
}

TEST_F(RudpCtrlReceiver, DecryptsNothingOnceStoppedDuringTheAcknowledgement) {
  auto stop = std::make_shared<test_support::StopState>();
  channel_.on_send([&] { stop->stopped = true; });
  deliver_enveloped_messages();
  const auto previous_calls = counting_.cipher_counters().size();
  const auto previous_sends = channel_.sent_packets().size();
  const auto cancelled = step_counting(42, platform::StopToken{stop});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), StepError::stopped);
  EXPECT_EQ(counting_.cipher_counters().size(), previous_calls);
  EXPECT_EQ(channel_.sent_packets().size(), previous_sends + 1);
}

TEST_F(RudpCtrlReceiver, AcknowledgesALateSessionRequestAsTransport) {
  std::vector<std::byte> late_request(88, std::byte{});
  late_request[0] = std::byte{0x06};
  late_request[6] = std::byte{0x5a};
  late_request[8] = std::byte{0x12};
  late_request[9] = std::byte{0x33};
  channel_.deliver(std::move(late_request));
  const auto result = step();
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value().front().delivery,
            remote_play::RudpCtrlDelivery::transport);
  EXPECT_EQ(channel_.sent(), ack_of(0x1234));
}

TEST_F(RudpCtrlReceiver, RefusesATruncatedSessionRequest) {
  channel_.deliver(std::vector<std::byte>{std::byte{0x06}, std::byte{},
                                          std::byte{}, std::byte{}});
  const auto truncated = step();
  ASSERT_FALSE(truncated);
  EXPECT_EQ(truncated.error(), StepError::malformed);
}

TEST_F(RudpCtrlReceiver, TimesOutWithoutTouchingAWaitingSwitch) {
  transport_switch::RemoteTransportSwitch state;
  ASSERT_TRUE(state.begin(7));
  const auto result = step();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), StepError::timeout);
  EXPECT_EQ(state.state(), transport_switch::State::waiting);
  EXPECT_TRUE(channel_.sent().empty());
}

TEST_F(RudpCtrlReceiver, ReportsASwitchNotificationWhateverTheAckSendDid) {
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode);
    SwitchChannel channel;
    channel.deliver(SwitchChannel::switch_packet());
    channel.fail_send(mode == 1);
    channel.short_send(mode == 2);
    transport_switch::RemoteTransportSwitch state;
    ASSERT_TRUE(state.begin(7));
    const auto result = step_on(channel);
    EXPECT_EQ(channel.sent(), ack_of(0x5678));
    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value().empty());
    EXPECT_TRUE(result.value().front().switch_notification);
    EXPECT_EQ(result.value().front().attempt_id, 7U);
    EXPECT_EQ(state.state(), transport_switch::State::waiting);
    EXPECT_TRUE(state.on_ack(attempt_id));
    EXPECT_EQ(state.state(), transport_switch::State::ready);
  }
}

}  // namespace
}  // namespace play_test
