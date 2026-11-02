#include "src/core/net/protocol/takion/ext_message_codec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

using Bytes = std::vector<std::byte>;

class ProtobufWriter {
 public:
  auto varint_field(std::uint32_t field, std::uint64_t value)
      -> ProtobufWriter& {
    varint(std::uint64_t{field} << 3U);
    varint(value);
    return *this;
  }

  auto int32_field(std::uint32_t field, std::int32_t value) -> ProtobufWriter& {
    return varint_field(
        field, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
  }

  auto double_field(std::uint32_t field, double value) -> ProtobufWriter& {
    varint((std::uint64_t{field} << 3U) | 1U);
    std::uint64_t bits{};
    std::memcpy(&bits, &value, sizeof bits);
    for (unsigned index = 0; index < 8; ++index)
      bytes_.push_back(static_cast<std::byte>(bits >> (8U * index)));
    return *this;
  }

  auto message_field(std::uint32_t field, const ProtobufWriter& body)
      -> ProtobufWriter& {
    varint((std::uint64_t{field} << 3U) | 2U);
    varint(body.bytes_.size());
    bytes_.insert(bytes_.end(), body.bytes_.begin(), body.bytes_.end());
    return *this;
  }

  auto bytes() const -> const Bytes& { return bytes_; }

 private:
  void varint(std::uint64_t value) {
    while (value >= 0x80U) {
      bytes_.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
      value >>= 7U;
    }
    bytes_.push_back(static_cast<std::byte>(value));
  }

  Bytes bytes_;
};

TEST(ExtReportCodec, EncodesAVideoFrameReportAsIndependentlyBuiltBytes) {
  const takion::VideoFrameMetricsReport report{
      7,    16666, -5,   1234,       54321, 0x12345, 0xfffe,
      true, false, 2000, 15'000'000, 0.125, 99};
  ProtobufWriter body;
  body.varint_field(1, 7);
  body.int32_field(2, 16666);
  body.int32_field(3, -5);
  body.int32_field(4, 1234);
  body.varint_field(5, 54321);
  body.varint_field(6, 0x12345);
  body.varint_field(7, 0xfffe);
  body.varint_field(8, 1);
  body.varint_field(9, 0);
  body.int32_field(10, 2000);
  body.varint_field(11, 15'000'000);
  body.double_field(12, 0.125);
  body.varint_field(13, 99);
  ProtobufWriter expected;
  expected.varint_field(1, 4);
  expected.message_field(6, body);

  const auto encoded =
      takion::ExtMessageCodec::encode_video_frame_metrics(report);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*encoded, expected.bytes());
  const auto decoded =
      takion::ExtMessageCodec::decode_video_frame_metrics(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, report);
  EXPECT_FALSE(takion::ExtMessageCodec::decode_received_tos_metrics(*encoded));
  EXPECT_FALSE(takion::ExtMessageCodec::decode_video_frame_metrics(
      Bytes(encoded->begin(), encoded->end() - 1)));
}

TEST(ExtReportCodec, DecodesTheInitialVideoFrameReport) {
  const takion::VideoFrameMetricsReport defaults{0, 16666};
  const auto initial =
      takion::ExtMessageCodec::encode_video_frame_metrics(defaults);
  ASSERT_TRUE(initial);
  const auto parsed =
      takion::ExtMessageCodec::decode_video_frame_metrics(*initial);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->send_int_us, 16666U);
  EXPECT_EQ(parsed->loss_ratio, 0.0);
}

TEST(ExtReportCodec, EncodesAReceivedTosReportAsIndependentlyBuiltBytes) {
  takion::ReceivedTosMetricsReport report;
  report.time_offset_msec = 1003;
  report.dscp_counts = {(0U << 24U) | 5U, (46U << 24U) | 0U};
  report.ecn_ect1_count = 1;
  report.ecn_ect0_count = 2;
  report.ecn_ce_count = 3;
  ProtobufWriter body;
  body.varint_field(1, 1003);
  body.varint_field(2, 5);
  body.varint_field(2, 46U << 24U);
  body.varint_field(3, 1);
  body.varint_field(4, 2);
  body.varint_field(5, 3);
  ProtobufWriter expected;
  expected.varint_field(1, 5);
  expected.message_field(7, body);

  const auto encoded =
      takion::ExtMessageCodec::encode_received_tos_metrics(report);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*encoded, expected.bytes());
  const auto decoded =
      takion::ExtMessageCodec::decode_received_tos_metrics(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, report);
}

TEST(ExtReportCodec, FitsTheLargestReceivedTosReportInOneMessage) {
  takion::ReceivedTosMetricsReport full;
  for (std::uint32_t dscp = 0; dscp < 64; ++dscp) {
    full.dscp_counts.push_back((dscp << 24U) | 0xffffffU);
    full.opposite_sent_dscp_counts.push_back((dscp << 24U) | 0xffffffU);
  }
  full.time_offset_msec = 0xffffffffU;
  full.ecn_ect1_count = full.ecn_ect0_count = full.ecn_ce_count = 0xffffffffU;
  const auto largest =
      takion::ExtMessageCodec::encode_received_tos_metrics(full);
  ASSERT_TRUE(largest);
  EXPECT_LE(largest->size(), takion::ext_message_payload_limit);
}

}  // namespace
}  // namespace takion_test
