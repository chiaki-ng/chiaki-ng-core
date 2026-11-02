#include "tests/unit/core/net/transport/rudp/frame_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace rudp_test {
namespace {

class RudpInitRequest : public FrameCodecRig {};

TEST_F(RudpInitRequest, MatchesTheGoldenPacket) {
  const auto result = FrameCodec::encode_init_request(0x1234, 0x89abcdefU);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), golden_init);
  EXPECT_EQ(result.value().size(), 22U);
}

TEST_F(RudpInitRequest, WritesTheHighestCounterDeterministically) {
  const auto result = FrameCodec::encode_init_request(0xFFFF, 0U);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()[8], std::byte{0xff});
  EXPECT_EQ(result.value()[9], std::byte{0xff});
  EXPECT_TRUE(FrameCodec::decode(result.value()).has_value());
  const auto again = FrameCodec::encode_init_request(0xFFFF, 0U);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again.value(), result.value());
}

class RudpInitResponse : public FrameCodecRig {
 protected:
  static auto response(const Bytes& payload) -> Bytes {
    return packet_of(0xD000, payload);
  }

  static auto zeroed() -> Bytes { return response(Bytes(8, std::byte{0x00})); }
};

TEST_F(RudpInitResponse, ReadsTheNextCounterAndTheCookie) {
  const Bytes payload = {std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
                         std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                         std::byte{0x00}, std::byte{0x00}, std::byte{0xaa},
                         std::byte{0xbb}};
  const auto result = FrameCodec::decode_init_response(response(payload));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().remote_counter, 2);
  EXPECT_EQ(result.value().cookie, (Bytes{std::byte{0xaa}, std::byte{0xbb}}));
}

TEST_F(RudpInitResponse, WrapsTheCounter) {
  const Bytes payload = {std::byte{0xff}, std::byte{0xff}, std::byte{0x00},
                         std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                         std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
  const auto result = FrameCodec::decode_init_response(response(payload));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().remote_counter, 0);
  EXPECT_EQ(result.value().cookie, Bytes{std::byte{0x01}});
}

TEST_F(RudpInitResponse, AcceptsAnEmptyCookie) {
  const auto result = FrameCodec::decode_init_response(zeroed());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result.value().cookie.empty());
}

TEST_F(RudpInitResponse, RefusesAPayloadUnderEightBytes) {
  const auto result = FrameCodec::decode_init_response(
      response({std::byte{0x00}, std::byte{0x01}, std::byte{0x00}}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpInitResponse, RefusesTheWrongFrameType) {
  Bytes wrong = zeroed();
  wrong[6] = std::byte{0x80};
  wrong[7] = std::byte{0x30};
  const auto result = FrameCodec::decode_init_response(wrong);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpInitResponse, RefusesATruncatedPacket) {
  Bytes truncated = zeroed();
  truncated.resize(truncated.size() - 1);
  const auto result = FrameCodec::decode_init_response(truncated);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpInitResponse, AcceptsConcatenatedFrames) {
  Bytes concatenated;
  append_frame(concatenated, 0xD000, Bytes(8, std::byte{0x00}));
  append_frame(concatenated, 0xD000, Bytes(8, std::byte{0x00}));
  EXPECT_TRUE(FrameCodec::decode_init_response(concatenated).has_value());
}

TEST_F(RudpInitResponse, IgnoresTheMagic) {
  Bytes magic = zeroed();
  magic[3] = std::byte{0x4e};
  EXPECT_TRUE(FrameCodec::decode_init_response(magic).has_value());
}

class RudpCookieRequest : public FrameCodecRig {};

TEST_F(RudpCookieRequest, MatchesTheGoldenPacket) {
  const Bytes cookie{std::byte{0xaa}, std::byte{0xbb}};
  const auto result =
      FrameCodec::encode_cookie_request(0x1234, 0x89abcdefU, cookie);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), golden_cookie);
  EXPECT_EQ(result.value().size(), 24U);
}

TEST_F(RudpCookieRequest, IsAnInitRequestOfItsOwnTypeWithoutACookie) {
  const auto result =
      FrameCodec::encode_cookie_request(0x1234, 0x89abcdefU, {});
  ASSERT_TRUE(result.has_value());
  Bytes expected = golden_init;
  expected[6] = std::byte{0x90};
  expected[7] = std::byte{0x30};
  EXPECT_EQ(result.value(), expected);
}

TEST_F(RudpCookieRequest, AcceptsTheLongestCookie) {
  const Bytes longest(4073, std::byte{0x41});
  const auto accept = FrameCodec::encode_cookie_request(1, 0U, longest);
  ASSERT_TRUE(accept.has_value());
  EXPECT_EQ(accept.value().size(), 8U + 14U + 4073U);
}

TEST_F(RudpCookieRequest, RefusesALongerCookie) {
  const Bytes too_big(4074, std::byte{0x41});
  const auto reject = FrameCodec::encode_cookie_request(1, 0U, too_big);
  ASSERT_FALSE(reject.has_value());
  EXPECT_EQ(reject.error(), FrameError::too_large);
}

class RudpCookieResponse : public FrameCodecRig {
 protected:
  static auto response(const Bytes& payload) -> Bytes {
    return packet_of(0xA030, payload);
  }
};

TEST_F(RudpCookieResponse, ReadsTheNextCounter) {
  const auto result = FrameCodec::decode_cookie_response(
      response({std::byte{0x00}, std::byte{0x01}}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().remote_counter, 2);
}

TEST_F(RudpCookieResponse, WrapsTheCounter) {
  const auto result = FrameCodec::decode_cookie_response(
      response({std::byte{0xff}, std::byte{0xff}}));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().remote_counter, 0);
}

TEST_F(RudpCookieResponse, RefusesAShortPayload) {
  const auto result =
      FrameCodec::decode_cookie_response(response({std::byte{0x00}}));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpCookieResponse, RefusesTheWrongFrameType) {
  Bytes wrong = response({std::byte{0x00}, std::byte{0x01}});
  wrong[6] = std::byte{0x80};
  wrong[7] = std::byte{0x30};
  const auto result = FrameCodec::decode_cookie_response(wrong);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpCookieResponse, AcceptsConcatenatedFrames) {
  Bytes concatenated;
  append_frame(concatenated, 0xA030, {std::byte{0x00}, std::byte{0x01}});
  append_frame(concatenated, 0xA030, {std::byte{0x00}, std::byte{0x01}});
  EXPECT_TRUE(FrameCodec::decode_cookie_response(concatenated).has_value());
}

TEST_F(RudpCookieResponse, RefusesATruncatedPacket) {
  Bytes truncated = response({std::byte{0x00}, std::byte{0x01}});
  truncated.resize(truncated.size() - 1);
  const auto result = FrameCodec::decode_cookie_response(truncated);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), FrameError::malformed);
}

TEST_F(RudpCookieResponse, IgnoresTheMagic) {
  Bytes magic = response({std::byte{0x00}, std::byte{0x01}});
  magic[3] = std::byte{0x4e};
  EXPECT_TRUE(FrameCodec::decode_cookie_response(magic).has_value());
}

}  // namespace
}  // namespace rudp_test
