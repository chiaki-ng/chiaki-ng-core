#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::CtrlCodec;

static_assert(!std::is_copy_constructible_v<remote_play::CtrlMessage>);

class CtrlFrameCodec : public ::testing::Test {
 protected:
  using Header = std::array<std::byte, 8>;

  static auto header(std::uint8_t b2, std::uint8_t b3, std::uint8_t b4,
                     std::uint8_t b5) -> Header {
    return {std::byte{0},  std::byte{0},  std::byte{b2}, std::byte{b3},
            std::byte{b4}, std::byte{b5}, std::byte{0},  std::byte{0}};
  }
};

TEST_F(CtrlFrameCodec, EncodesHeadersExactlyLikeTheReference) {
  const auto session_id = CtrlCodec::encode_frame_header(0x33, 0);
  ASSERT_TRUE(session_id);
  EXPECT_EQ(session_id.value(), header(0, 0, 0, 0x33));
  const auto heartbeat = CtrlCodec::encode_frame_header(0xfe, 2);
  ASSERT_TRUE(heartbeat);
  EXPECT_EQ(heartbeat.value(), header(0, 2, 0, 0xfe));
  const auto login_pin = CtrlCodec::encode_frame_header(0x8004, 504);
  ASSERT_TRUE(login_pin);
  EXPECT_EQ(login_pin.value(), header(1, 0xf8, 0x80, 4));
  const auto over = CtrlCodec::encode_frame_header(0x23, 578);
  ASSERT_TRUE(over);
  EXPECT_EQ(over.value(), header(2, 0x42, 0, 0x23));
}

TEST_F(CtrlFrameCodec, EncodesTheWidestLengthAndRefusesAWiderOne) {
  const auto widest = CtrlCodec::encode_frame_header(0x23, 0xffffffffU);
  ASSERT_TRUE(widest);
  EXPECT_EQ(widest.value()[0], std::byte{0xff});
  EXPECT_EQ(widest.value()[3], std::byte{0xff});
  if constexpr (sizeof(std::size_t) > 4) {
    const auto wrapped = CtrlCodec::encode_frame_header(
        0x23, static_cast<std::size_t>(0xffffffffU) + 1U);
    ASSERT_FALSE(wrapped);
    EXPECT_EQ(wrapped.error(), remote_play::CtrlFrameError::too_large);
  }
}

TEST_F(CtrlFrameCodec, DecodesAFrameOnlyOnceItIsComplete) {
  const std::array<std::byte, 11> wire{
      std::byte{0},    std::byte{0},    std::byte{0},   std::byte{2},
      std::byte{0},    std::byte{0xfe}, std::byte{0},   std::byte{0},
      std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};
  for (std::size_t n = 0; n < 10; ++n) {
    const auto partial = CtrlCodec::decode_frame(std::span(wire).first(n));
    ASSERT_FALSE(partial) << "prefix " << n;
    EXPECT_EQ(partial.error(), remote_play::CtrlFrameError::incomplete)
        << "prefix " << n;
  }
  const auto result = CtrlCodec::decode_frame(wire);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().type, 0xfe);
  EXPECT_EQ(result.value().consumed, 10U);
  ASSERT_EQ(result.value().payload.size(), 2U);
  EXPECT_EQ(result.value().payload[0], std::byte{0xaa});
  EXPECT_EQ(result.value().payload[1], std::byte{0xbb});
}

TEST_F(CtrlFrameCodec, DecodesTheLargestFrameAndRefusesALargerOne) {
  const auto max_header = CtrlCodec::encode_frame_header(0x33, 504);
  ASSERT_TRUE(max_header);
  std::vector<std::byte> max(max_header.value().begin(),
                             max_header.value().end());
  max.resize(512, std::byte{0x55});
  const auto max_result = CtrlCodec::decode_frame(max);
  ASSERT_TRUE(max_result);
  EXPECT_EQ(max_result.value().consumed, 512U);
  const auto invalid = CtrlCodec::decode_frame(header(1, 0xf9, 0, 0x33));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), remote_play::CtrlFrameError::too_large);
}

TEST_F(CtrlFrameCodec, IgnoresTheReservedBytes) {
  const auto session_id = CtrlCodec::encode_frame_header(0x33, 0);
  ASSERT_TRUE(session_id);
  auto reserved = session_id.value();
  reserved[6] = std::byte{1};
  const auto ignored = CtrlCodec::decode_frame(reserved);
  ASSERT_TRUE(ignored);
  EXPECT_EQ(ignored.value().type, 0x33);
}

class CtrlMessageCodec : public ::testing::Test {
 protected:
  static auto id_payload(bool valid) -> std::vector<std::byte> {
    std::vector<std::byte> payload{std::byte{0x4a}};
    payload.insert(payload.end(), 24, std::byte{'A'});
    if (!valid) payload[12] = std::byte{'-'};
    return payload;
  }

  static constexpr std::array<std::byte, 1> nonempty{std::byte{9}};
};

TEST_F(CtrlMessageCodec, DecodesAValidSessionId) {
  const auto good = CtrlCodec::decode_message(0x33, id_payload(true));
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlSessionId>(good));
  EXPECT_EQ(std::get<remote_play::CtrlSessionId>(good).value.view(),
            std::string(24, 'A'));
}

TEST_F(CtrlMessageCodec, FallsBackOnASessionIdWithAnInvalidCharacter) {
  const auto bad = CtrlCodec::decode_message(0x33, id_payload(false));
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlSessionIdFallback>(bad));
  EXPECT_EQ(std::get<remote_play::CtrlSessionIdFallback>(bad).reason,
            remote_play::SessionIdError::invalid_character);
}

TEST_F(CtrlMessageCodec, FallsBackOnAnEmptySessionId) {
  const auto empty = CtrlCodec::decode_message(0x33, {});
  ASSERT_TRUE(
      std::holds_alternative<remote_play::CtrlSessionIdFallback>(empty));
  EXPECT_EQ(std::get<remote_play::CtrlSessionIdFallback>(empty).reason,
            remote_play::SessionIdError::too_short);
}

TEST_F(CtrlMessageCodec, AcceptsASessionIdWithAnUnusualMarker) {
  auto unusual_marker = id_payload(true);
  unusual_marker[0] = std::byte{0};
  const auto accepted = CtrlCodec::decode_message(0x33, unusual_marker);
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlSessionId>(accepted));
  EXPECT_TRUE(
      std::get<remote_play::CtrlSessionId>(accepted).value.unexpected_marker());
}

TEST_F(CtrlMessageCodec, DecodesHeartbeatsAndNotesAnUnexpectedPayload) {
  const auto heartbeat = CtrlCodec::decode_message(0xfe, {});
  ASSERT_TRUE(
      std::holds_alternative<remote_play::CtrlHeartbeatRequest>(heartbeat));
  EXPECT_FALSE(std::get<remote_play::CtrlHeartbeatRequest>(heartbeat)
                   .unexpected_payload);
  const auto heartbeat_extra = CtrlCodec::decode_message(0xfe, nonempty);
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlHeartbeatRequest>(
      heartbeat_extra));
  EXPECT_TRUE(std::get<remote_play::CtrlHeartbeatRequest>(heartbeat_extra)
                  .unexpected_payload);
}

TEST_F(CtrlMessageCodec, DecodesPinRequestsAndSwitchAcksWithTheirPayloads) {
  const auto pin = CtrlCodec::decode_message(0x4, nonempty);
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlLoginPinRequest>(pin));
  EXPECT_TRUE(
      std::get<remote_play::CtrlLoginPinRequest>(pin).unexpected_payload);
  const auto switch_ack = CtrlCodec::decode_message(0x34, nonempty);
  ASSERT_TRUE(
      std::holds_alternative<remote_play::CtrlStreamSwitchAck>(switch_ack));
  EXPECT_TRUE(std::get<remote_play::CtrlStreamSwitchAck>(switch_ack)
                  .unexpected_payload);
}

TEST_F(CtrlMessageCodec, DecodesLoginResultsAndIgnoresUnknownStates) {
  constexpr std::array<std::byte, 2> success{std::byte{0}, std::byte{0xff}};
  constexpr std::array<std::byte, 1> incorrect{std::byte{1}};
  constexpr std::array<std::byte, 1> unknown_state{std::byte{2}};
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlLoginSuccess>(
      CtrlCodec::decode_message(0x5, success)));
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlLoginPinIncorrect>(
      CtrlCodec::decode_message(0x5, incorrect)));
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlIgnoredMessage>(
      CtrlCodec::decode_message(0x5, {})));
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlIgnoredMessage>(
      CtrlCodec::decode_message(0x5, unknown_state)));
}

TEST_F(CtrlMessageCodec, IgnoresAnUnknownTypeAndKeepsIt) {
  const std::array<std::byte, 1> extra{std::byte{0xaa}};
  const auto other = CtrlCodec::decode_message(0xbeef, extra);
  ASSERT_TRUE(std::holds_alternative<remote_play::CtrlIgnoredMessage>(other));
  EXPECT_EQ(std::get<remote_play::CtrlIgnoredMessage>(other).type, 0xbeef);
}

}  // namespace
}  // namespace remote_play_test
