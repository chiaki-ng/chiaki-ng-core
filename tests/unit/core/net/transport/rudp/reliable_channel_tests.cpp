#include "tests/support/stop_states.hpp"
#include "tests/unit/core/net/transport/rudp/rudp_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace rudp_test {
namespace {

using libchiaking::net::transport::rudp::FrameCodec;
using libchiaking::net::transport::rudp::ReliableChannel;
using libchiaking::net::transport::rudp::SendBuffer;
namespace datagram = libchiaking::net::transport::datagram;
namespace pf = libchiaking::platform;

class TimedChannel final
    : public libchiaking::net::transport::rudp::HandshakeChannel {
 public:
  explicit TimedChannel(test_support::FakeClock& source) : clock_(&source) {}

  auto send(std::span<const std::byte> bytes)
      -> libchiaking::Expected<std::size_t, datagram::TransportError> override {
    sent.emplace_back(bytes.begin(), bytes.end());
    times.push_back(clock_->now_us);
    if (fail)
      return libchiaking::unexpected(datagram::TransportError::io_failure);
    return short_send ? std::size_t{0} : bytes.size();
  }

  auto receive_for(std::chrono::microseconds duration, const pf::StopToken&)
      -> libchiaking::Expected<std::optional<datagram::OwnedDatagram>,
                               datagram::TransportError> override {
    if (receive_fail)
      return libchiaking::unexpected(datagram::TransportError::io_failure);
    if (inbox) return std::exchange(inbox, std::nullopt);
    clock_->now_us += static_cast<std::uint64_t>(duration.count());
    if (stop) stop->stopped = true;
    return std::optional<datagram::OwnedDatagram>{};
  }

  std::shared_ptr<test_support::StopState> stop;
  std::optional<datagram::OwnedDatagram> inbox;
  std::vector<datagram::OwnedDatagram> sent;
  std::vector<std::uint64_t> times;
  bool fail{};
  bool short_send{};
  bool receive_fail{};

 private:
  test_support::FakeClock* clock_;
};

class RudpReliableChannelTest : public ::testing::Test {
 protected:
  using Frame = libchiaking::net::transport::rudp::Frame;

  static auto packet(std::uint16_t sequence) -> datagram::OwnedDatagram {
    const Frame frame{
        0x0230,
        {static_cast<std::byte>(sequence >> 8U),
         static_cast<std::byte>(sequence), std::byte{0xde}, std::byte{0xad}}};
    return FrameCodec::encode({&frame, 1}).value();
  }

  auto acknowledge(ReliableChannel& target, std::uint16_t sequence)
      -> testing::AssertionResult {
    raw.inbox = FrameCodec::encode_ack_message(123, sequence).value();
    const auto received = target.receive_for(std::chrono::microseconds{1}, {});
    if (!received) return testing::AssertionFailure() << "the receive failed";
    if (!received.value())
      return testing::AssertionFailure() << "the acknowledgement was not read";
    return testing::AssertionSuccess();
  }

  auto acknowledge(std::uint16_t sequence) -> testing::AssertionResult {
    return acknowledge(channel, sequence);
  }

  test_support::FakeClock clock{0};
  TimedChannel raw{clock};
  SendBuffer pending;
  ReliableChannel channel{raw, pending, clock};
};

class RudpReliableChannelResend : public RudpReliableChannelTest {};

TEST_F(RudpReliableChannelResend, WaitsOneRetransmitIntervalAfterAQuickAck) {
  ASSERT_TRUE(channel.send(packet(1)));
  clock.now_us = 50000;
  EXPECT_TRUE(acknowledge(2));
  EXPECT_EQ(pending.size(), 0U);
  clock.now_us = 100000;
  ASSERT_TRUE(channel.send(packet(3)));
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{700000}, {}));
  EXPECT_EQ(raw.sent.size(), 3U);
  ASSERT_FALSE(raw.times.empty());
  EXPECT_EQ(raw.times.back(), 600000U);
}

TEST_F(RudpReliableChannelResend, CountsTheIntervalFromALaterSend) {
  ASSERT_TRUE(channel.send(packet(1)));
  clock.now_us = 50000;
  EXPECT_TRUE(acknowledge(2));
  clock.now_us = 250000;
  ASSERT_TRUE(channel.send(packet(3)));
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{700000}, {}));
  EXPECT_EQ(raw.sent.size(), 3U);
  ASSERT_FALSE(raw.times.empty());
  EXPECT_EQ(raw.times.back(), 850000U);
}

TEST_F(RudpReliableChannelResend, ResumesPendingPacketsInANewChannel) {
  const auto bytes = packet(9);
  {
    ReliableChannel first_channel{raw, pending, clock};
    ASSERT_TRUE(first_channel.send(bytes));
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_TRUE(
        first_channel.receive_for(std::chrono::microseconds{400000}, {}));
    EXPECT_EQ(raw.sent.size(), 1U);
  }
  ReliableChannel resumed{raw, pending, clock};
  EXPECT_TRUE(resumed.receive_for(std::chrono::microseconds{200000}, {}));
  ASSERT_EQ(raw.sent.size(), 2U);
  EXPECT_EQ(raw.times.back(), 600000U);
  EXPECT_EQ(raw.sent.front(), raw.sent.back());
  EXPECT_TRUE(acknowledge(resumed, 9));
  EXPECT_EQ(pending.size(), 1U);
  EXPECT_TRUE(acknowledge(resumed, 10));
  EXPECT_EQ(pending.size(), 0U);
  EXPECT_TRUE(acknowledge(resumed, 10));
  EXPECT_TRUE(resumed.receive_for(std::chrono::microseconds{1000000}, {}));
  EXPECT_EQ(raw.sent.size(), 2U);
}

TEST_F(RudpReliableChannelResend, KeepsResendingEveryIntervalUntilItGivesUp) {
  ASSERT_TRUE(channel.send(packet(0)));
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{16000000}, {}));
  ASSERT_EQ(raw.sent.size(), 26U);
  EXPECT_EQ(pending.size(), 0U);
  for (std::size_t index = 0; index < raw.sent.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(raw.times[index], index * 600000);
    EXPECT_EQ(raw.sent[index], raw.sent.front());
  }
}

TEST_F(RudpReliableChannelResend, ResendsOnlyOnceTheIntervalHasPassed) {
  ASSERT_TRUE(channel.send(packet(4)));
  clock.now_us = 400500;
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{1}, {}));
  EXPECT_EQ(raw.sent.size(), 1U);
  clock.now_us = 600500;
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{1}, {}));
  EXPECT_EQ(raw.sent.size(), 2U);
}

TEST_F(RudpReliableChannelResend, StopsResendingOnceStopped) {
  ASSERT_TRUE(channel.send(packet(1)));
  raw.stop = std::make_shared<test_support::StopState>();
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{1000000},
                                  pf::StopToken{raw.stop}));
  EXPECT_EQ(raw.sent.size(), 1U);
  clock.now_us = 2000000;
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{1},
                                  pf::StopToken{raw.stop}));
  EXPECT_EQ(raw.sent.size(), 1U);
}

class RudpReliableChannelAcks : public RudpReliableChannelTest {};

TEST_F(RudpReliableChannelAcks, ReleasesAcrossTheSequenceWrap) {
  for (const auto sequence : {65534, 65535, 0, 1})
    ASSERT_TRUE(channel.send(packet(static_cast<std::uint16_t>(sequence))));
  EXPECT_TRUE(acknowledge(0));
  EXPECT_EQ(pending.size(), 2U);
  EXPECT_TRUE(acknowledge(65535));
  EXPECT_EQ(pending.size(), 2U);
  EXPECT_TRUE(acknowledge(2));
  EXPECT_EQ(pending.size(), 0U);
}

TEST_F(RudpReliableChannelAcks, IgnoresAnAckHalfTheSequenceSpaceAhead) {
  ASSERT_TRUE(channel.send(packet(10)));
  EXPECT_TRUE(acknowledge(static_cast<std::uint16_t>(11 + 0x8000)));
  EXPECT_EQ(pending.size(), 1U);
}

TEST_F(RudpReliableChannelAcks, KeepsAtMostTheBufferCapacity) {
  for (std::uint16_t sequence = 0; sequence < SendBuffer::capacity; ++sequence)
    ASSERT_TRUE(channel.send(packet(sequence)));
  EXPECT_TRUE(channel.send(packet(16)));
  EXPECT_EQ(raw.sent.size(), 17U);
  EXPECT_EQ(pending.size(), 16U);
  EXPECT_TRUE(channel.send(packet(0)));
  EXPECT_EQ(raw.sent.size(), 18U);
  EXPECT_EQ(pending.size(), 16U);
  const auto only_ack = FrameCodec::encode_ack_message(1, 3).value();
  EXPECT_TRUE(channel.send(only_ack));
  EXPECT_EQ(pending.size(), 16U);
  EXPECT_TRUE(acknowledge(8));
  EXPECT_EQ(pending.size(), 8U);
  EXPECT_TRUE(channel.send(packet(16)));
}

TEST_F(RudpReliableChannelAcks, ReadsAnAckCarriedByAnyAckSubtype) {
  for (const auto subtype : {0x1230, 0x2630, 0x3630, 0x2430}) {
    SCOPED_TRACE(subtype);
    test_support::FakeClock subtype_clock{0};
    TimedChannel subtype_raw{subtype_clock};
    SendBuffer subtype_pending;
    ReliableChannel subtype_channel{subtype_raw, subtype_pending,
                                    subtype_clock};
    ASSERT_TRUE(subtype_channel.send(packet(8)));
    ASSERT_TRUE(subtype_channel.send(packet(9)));
    const std::vector<Frame> frames{
        {static_cast<std::uint16_t>(subtype),
         {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{9}}},
        {0x2030, {std::byte{0}, std::byte{2}, std::byte{0}, std::byte{10}}},
    };
    subtype_raw.inbox = FrameCodec::encode(frames).value();
    EXPECT_TRUE(subtype_channel.receive_for(std::chrono::microseconds{1}, {}));
    EXPECT_EQ(subtype_pending.size(), 1U);
  }
}

TEST_F(RudpReliableChannelAcks, RetiresNothingOnAFrameThatIsNoAck) {
  ASSERT_TRUE(channel.send(packet(0x8001)));
  ASSERT_TRUE(channel.send(packet(0x8002)));
  const Frame other{0x2030,
                    {std::byte{0}, std::byte{2}, std::byte{0}, std::byte{0}}};
  raw.inbox = FrameCodec::encode({&other, 1}).value();
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{1}, {}));
  EXPECT_EQ(pending.size(), 2U);
}

class RudpReliableChannelFailures : public RudpReliableChannelTest {};

TEST_F(RudpReliableChannelFailures, KeepsNothingWhenTheSendFails) {
  raw.fail = true;
  const auto sent = channel.send(packet(0));
  EXPECT_EQ(pending.size(), 0U);
  EXPECT_FALSE(sent);
}

TEST_F(RudpReliableChannelFailures, KeepsNothingAfterAShortSend) {
  raw.short_send = true;
  const auto sent = channel.send(packet(0));
  EXPECT_EQ(pending.size(), 0U);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), 0U);
}

TEST_F(RudpReliableChannelFailures, KeepsThePacketWhenAResendFails) {
  EXPECT_TRUE(channel.send(packet(0)));
  EXPECT_EQ(pending.size(), 1U);
  raw.fail = true;
  EXPECT_TRUE(channel.receive_for(std::chrono::microseconds{600000}, {}));
  EXPECT_EQ(pending.size(), 1U);
  EXPECT_EQ(raw.sent.size(), 2U);
  raw.receive_fail = true;
  EXPECT_FALSE(channel.receive_for(std::chrono::microseconds{1}, {}));
}

}  // namespace
}  // namespace rudp_test
