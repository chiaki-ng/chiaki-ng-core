#include "tests/unit/core/net/transport/rudp/rudp_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rudp_test {
namespace {

using libchiaking::net::transport::rudp::Handshake;
using HState = libchiaking::net::transport::rudp::HandshakeState;
using HError = libchiaking::net::transport::rudp::HandshakeError;
using Bytes = RudpPackets::Bytes;

class RudpHandshakeSteps : public ::testing::Test, protected RudpPackets {
 protected:
  template <class Step>
  static auto sends(const Step& step) -> bool {
    return step && step->has_value();
  }
};

TEST_F(RudpHandshakeSteps, BeginsBySendingAnInitRequest) {
  Handshake handshake(1, 0x1234, 0x89abcdefU);
  EXPECT_EQ(handshake.state(), HState::idle);
  EXPECT_FALSE(handshake.remote_counter().has_value());
  const auto step = handshake.begin();
  EXPECT_FALSE(handshake.remote_counter().has_value());
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  ASSERT_TRUE(step.has_value());
  ASSERT_TRUE(step->has_value());
  EXPECT_EQ((**step)[6], std::byte{0x80});
  EXPECT_EQ((**step)[7], std::byte{0x30});
  EXPECT_EQ(handshake.sends_for_stage(), 1);
  const auto again = handshake.begin();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), HError::invalid_transition);
}

TEST_F(RudpHandshakeSteps, CompletesThroughInitAndCookie) {
  Handshake handshake(7, 1, 2);
  auto step = handshake.begin();
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  step = handshake.on_packet(
      7, init_response(9, {std::byte{0xaa}, std::byte{0xbb}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[6], std::byte{0x90});
  EXPECT_EQ((**step)[7], std::byte{0x30});
  EXPECT_EQ(handshake.sends_for_stage(), 1);
  step = handshake.on_packet(7, cookie_response(10));
  EXPECT_EQ(handshake.state(), HState::established);
  EXPECT_FALSE(sends(step));
  ASSERT_TRUE(handshake.remote_counter().has_value());
  EXPECT_EQ(*handshake.remote_counter(), 11);
  step = handshake.on_packet(7, cookie_response(10));
  EXPECT_EQ(handshake.state(), HState::established);
  EXPECT_FALSE(sends(step));
}

TEST_F(RudpHandshakeSteps, IgnoresAPacketFromAStaleAttempt) {
  Handshake handshake(3, 1, 0);
  (void)handshake.begin();
  const auto step = handshake.on_packet(2, init_response(0, {std::byte{0x01}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  EXPECT_FALSE(sends(step));
  EXPECT_EQ(handshake.sends_for_stage(), 1);
}

TEST_F(RudpHandshakeSteps, SpendsATryAndResendsOnAMismatchedPacket) {
  Handshake handshake(3, 1, 0);
  (void)handshake.begin();
  auto step = handshake.on_packet(3, cookie_response(0));
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[6], std::byte{0x80});
  EXPECT_EQ((**step)[7], std::byte{0x30});
  EXPECT_EQ((**step)[8], std::byte{0x00});
  EXPECT_EQ((**step)[9], std::byte{0x02});
  EXPECT_EQ(handshake.sends_for_stage(), 2);
  step = handshake.on_packet(
      3, encode_one(Frame{
             0xD000, {std::byte{0x00}, std::byte{0x04}, std::byte{0x00}}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[9], std::byte{0x03});
  EXPECT_EQ(handshake.sends_for_stage(), 3);
  step = handshake.on_packet(3, Bytes(16, std::byte{0xff}));
  EXPECT_EQ(handshake.state(), HState::failed);
  ASSERT_FALSE(step);
  EXPECT_EQ(step.error(), HError::retries_exhausted);
  EXPECT_FALSE(sends(step));
}

TEST_F(RudpHandshakeSteps, ResendsTheCookieForADuplicateInitResponse) {
  Handshake handshake(3, 1, 0);
  (void)handshake.begin();
  auto step = handshake.on_packet(3, init_response(4, {std::byte{0x55}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[9], std::byte{0x02});
  step = handshake.on_packet(3, init_response(4, {std::byte{0x55}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[6], std::byte{0x90});
  EXPECT_EQ((**step)[7], std::byte{0x30});
  EXPECT_EQ((**step)[9], std::byte{0x03});
  EXPECT_EQ(handshake.sends_for_stage(), 2);
  step = handshake.on_packet(3, cookie_response(9));
  EXPECT_EQ(handshake.state(), HState::established);
  ASSERT_TRUE(handshake.remote_counter().has_value());
  EXPECT_EQ(*handshake.remote_counter(), 10);
  EXPECT_EQ(handshake.next_counter(), 4);
}

TEST_F(RudpHandshakeSteps, AbortsOnADatagramShorterThanAFrame) {
  for (std::size_t size = 0; size <= 8; ++size) {
    SCOPED_TRACE(size);
    Handshake handshake(3, 1, 0);
    (void)handshake.begin();
    const auto step = handshake.on_packet(3, Bytes(size, std::byte{0xc0}));
    EXPECT_EQ(handshake.state(), HState::failed);
    ASSERT_FALSE(step);
    EXPECT_EQ(step.error(), HError::network);
    EXPECT_FALSE(sends(step));
  }
}

TEST_F(RudpHandshakeSteps, AcceptsAnInitResponseWithABadMagic) {
  Handshake handshake(3, 1, 0);
  (void)handshake.begin();
  auto packet = init_response(4, {std::byte{0x55}});
  packet[2] = std::byte{0x00};
  const auto step = handshake.on_packet(3, packet);
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  EXPECT_TRUE(sends(step));
}

TEST_F(RudpHandshakeSteps, RetriesOnTimeoutThenFails) {
  Handshake handshake(5, 1, 0);
  (void)handshake.begin();
  auto step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ(handshake.sends_for_stage(), 2);
  EXPECT_EQ((**step)[8], std::byte{0x00});
  EXPECT_EQ((**step)[9], std::byte{0x02});
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::awaiting_init);
  EXPECT_TRUE(sends(step));
  EXPECT_EQ(handshake.sends_for_stage(), 3);
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::failed);
  ASSERT_FALSE(step);
  EXPECT_EQ(step.error(), HError::retries_exhausted);
  EXPECT_FALSE(sends(step));
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::failed);
}

TEST_F(RudpHandshakeSteps, ResetsTheTimeoutCountOnEachStage) {
  Handshake handshake(6, 1, 0);
  (void)handshake.begin();
  auto step = handshake.on_timeout();
  EXPECT_TRUE(sends(step));
  step = handshake.on_packet(6, init_response(2, {std::byte{0x01}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  EXPECT_EQ(handshake.sends_for_stage(), 1);
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.sends_for_stage(), 2);
  step = handshake.on_packet(6, cookie_response(3));
  EXPECT_EQ(handshake.state(), HState::established);
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::established);
  EXPECT_FALSE(sends(step));
}

TEST_F(RudpHandshakeSteps, StaysCancelled) {
  Handshake handshake(9, 1, 0);
  (void)handshake.begin();
  const auto cancelled = handshake.cancel();
  EXPECT_TRUE(cancelled.has_value());
  EXPECT_EQ(handshake.state(), HState::cancelled);
  EXPECT_FALSE(handshake.remote_counter().has_value());
  auto step = handshake.on_packet(9, init_response(0, {std::byte{0x01}}));
  EXPECT_EQ(handshake.state(), HState::cancelled);
  EXPECT_FALSE(sends(step));
  step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::cancelled);
  const auto again = handshake.cancel();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), HError::invalid_transition);
}

TEST_F(RudpHandshakeSteps, WrapsBothCounters) {
  Handshake handshake(11, 0xFFFF, 0);
  auto step = handshake.begin();
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[8], std::byte{0xff});
  EXPECT_EQ((**step)[9], std::byte{0xff});
  EXPECT_EQ(handshake.next_counter(), 0);
  step = handshake.on_packet(11, init_response(0xFFFE, {std::byte{0x01}}));
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  ASSERT_TRUE(sends(step));
  EXPECT_EQ((**step)[8], std::byte{0x00});
  EXPECT_EQ((**step)[9], std::byte{0x00});
  EXPECT_EQ(handshake.next_counter(), 1);
  step = handshake.on_packet(11, cookie_response(0xFFFF));
  EXPECT_EQ(handshake.state(), HState::established);
  ASSERT_TRUE(handshake.remote_counter().has_value());
  EXPECT_EQ(*handshake.remote_counter(), 0);
}

TEST_F(RudpHandshakeSteps, ReadsACombinedAnswerForTheAwaitedStage) {
  auto combined = init_response(4, {std::byte{0x55}});
  const auto cookie = cookie_response(9);
  combined.insert(combined.end(), cookie.begin(), cookie.end());
  Handshake handshake(3, 1, 0);
  (void)handshake.begin();
  auto step = handshake.on_packet(3, combined);
  EXPECT_EQ(handshake.state(), HState::awaiting_cookie);
  EXPECT_TRUE(sends(step));
  step = handshake.on_packet(3, combined);
  EXPECT_EQ(handshake.state(), HState::established);
  EXPECT_FALSE(sends(step));
  ASSERT_TRUE(handshake.remote_counter().has_value());
  EXPECT_EQ(*handshake.remote_counter(), 10);
}

TEST_F(RudpHandshakeSteps, LeavesNoTraceOfEventsBeforeItBegins) {
  Handshake handshake(4, 1, 0);
  auto step = handshake.on_timeout();
  EXPECT_EQ(handshake.state(), HState::idle);
  ASSERT_TRUE(step);
  EXPECT_FALSE(step->has_value());
  step = handshake.on_packet(4, init_response(2, {std::byte{0x01}}));
  EXPECT_EQ(handshake.state(), HState::idle);
  ASSERT_TRUE(step);
  EXPECT_FALSE(step->has_value());
  EXPECT_EQ(handshake.next_counter(), 1);
  EXPECT_EQ(handshake.sends_for_stage(), 0);
  const auto cancelled = handshake.cancel();
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(handshake.state(), HState::cancelled);
  const auto restarted = handshake.begin();
  ASSERT_FALSE(restarted);
  EXPECT_EQ(restarted.error(), HError::invalid_transition);
}

class RudpHandshakeParameters : public ::testing::Test {
 protected:
  ScriptedRandom random;
};

TEST_F(RudpHandshakeParameters, StartTheCounterAtItsFloorForZeroBytes) {
  random.words(0, 0);
  const auto parameters = Handshake::generate_parameters(random);
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters.value().counter, 0x1FF);
  EXPECT_EQ(parameters.value().header, 0x8000);
  EXPECT_EQ(random.calls, 1);
}

TEST_F(RudpHandshakeParameters, WrapTheHeaderForTheLargestBytes) {
  random.words(0xFFFFFFFFU, 0xFFFFFFFFU);
  const auto parameters = Handshake::generate_parameters(random);
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters.value().counter, 0x3FE);
  EXPECT_EQ(parameters.value().header, 0x7FFF);
}

TEST_F(RudpHandshakeParameters, KeepTheCounterBelowTheModulus) {
  random.words(0x5DFF, 0x1234);
  const auto parameters = Handshake::generate_parameters(random);
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters.value().counter, 0x5FFE);
  EXPECT_EQ(parameters.value().header, 0x9234);
}

TEST_F(RudpHandshakeParameters, PassOnARandomFailure) {
  random.fail = true;
  const auto parameters = Handshake::generate_parameters(random);
  ASSERT_FALSE(parameters.has_value());
  EXPECT_EQ(parameters.error(),
            libchiaking::platform::PlatformError::io_failure);
  EXPECT_EQ(random.calls, 1);
}

}  // namespace
}  // namespace rudp_test
