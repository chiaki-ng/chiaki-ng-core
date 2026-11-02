#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class TransportFrameControl : public WireVectorsTest {
 protected:
  static auto raw(std::string_view header, std::size_t payload)
      -> Result<ControlFragment> {
    auto record = test_support::Bytes::unhex(header);
    record.insert(record.end(), payload, std::byte{0x7b});
    return TransportFrameCodec::decode_control(record);
  }
};

class TransportFrameControlAck : public TransportFrameControl {};

TEST_F(TransportFrameControl, HelloMatchesTheVector) {
  const auto transcript =
      test_support::Bytes::unhex(test_support::kakusan::Fixtures::string(
          test_support::kakusan::Fixtures::text("vectors.json"), "transcript"));
  const auto expected =
      test_support::Bytes::unhex(vectors.text("control_hello_hex"));
  const auto decoded = TransportFrameCodec::decode_control(expected);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().kind, ControlKind::data);
  EXPECT_EQ(decoded.value().message_id, 1U);
  EXPECT_EQ(decoded.value().index, 0U);
  EXPECT_EQ(decoded.value().count, 1U);

  const std::vector<std::byte> message(decoded.value().payload.begin(),
                                       decoded.value().payload.end());
  const auto fragments =
      TransportFrameCodec::encode_control_fragments(1, message);
  ASSERT_TRUE(fragments);
  ASSERT_EQ(fragments.value().size(), 1U);
  EXPECT_EQ(fragments.value()[0], expected);
  EXPECT_FALSE(transcript.empty());
}

TEST_F(TransportFrameControlAck, MatchesTheVector) {
  const auto ack = TransportFrameCodec::encode_control_ack(1);
  EXPECT_EQ(test_support::Bytes::hex(ack), vectors.text("control_ack_hex"));
  const auto decoded_ack = TransportFrameCodec::decode_control(ack);
  ASSERT_TRUE(decoded_ack);
  EXPECT_EQ(decoded_ack.value().kind, ControlKind::ack);
  EXPECT_EQ(decoded_ack.value().message_id, 1U);
}

TEST_F(TransportFrameControlAck, WritesTheMessageIdBigEndian) {
  EXPECT_EQ(test_support::Bytes::hex(
                TransportFrameCodec::encode_control_ack(0x01020304U)),
            "52434331020000000102030400000000");
}

TEST_F(TransportFrameControl, FragmentsTheLargestMessageAndRebuildsIt) {
  std::vector<std::byte> message(max_control_message_size);
  for (std::size_t index = 0; index < message.size(); ++index)
    message[index] = static_cast<std::byte>(index % 251);
  const auto fragments =
      TransportFrameCodec::encode_control_fragments(0xffffffffU, message);
  ASSERT_TRUE(fragments);
  ASSERT_EQ(fragments.value().size(), max_control_fragments);
  std::vector<std::byte> rebuilt;
  for (std::size_t index = 0; index < fragments.value().size(); ++index) {
    SCOPED_TRACE(index);
    const auto decoded =
        TransportFrameCodec::decode_control(fragments.value()[index]);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().index, index);
    EXPECT_EQ(decoded.value().count, max_control_fragments);
    EXPECT_EQ(decoded.value().message_id, 0xffffffffU);
    EXPECT_EQ(decoded.value().payload.size(), index + 1 == max_control_fragments
                                                  ? std::size_t{512}
                                                  : control_fragment_size);
    rebuilt.insert(rebuilt.end(), decoded.value().payload.begin(),
                   decoded.value().payload.end());
  }
  EXPECT_EQ(rebuilt, message);
}

TEST_F(TransportFrameControl, CountsFragmentsAtTheEdges) {
  EXPECT_EQ(TransportFrameCodec::control_fragment_count(1), 1U);
  EXPECT_EQ(TransportFrameCodec::control_fragment_count(960), 1U);
  EXPECT_EQ(TransportFrameCodec::control_fragment_count(961), 2U);
}

TEST_F(TransportFrameControl, RefusesAZeroIdAnEmptyOrAnOversizedMessage) {
  std::vector<std::byte> message(max_control_message_size);
  EXPECT_FALSE(TransportFrameCodec::encode_control_fragments(0, message));
  EXPECT_FALSE(TransportFrameCodec::encode_control_fragments(1, {}));
  message.push_back(std::byte{});
  EXPECT_FALSE(TransportFrameCodec::encode_control_fragments(1, message));
}

TEST_F(TransportFrameControl, DecodesOnlyWellFormedDataRecords) {
  EXPECT_TRUE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0001"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0001"
          "0002",
          3));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0001"
          "0002",
          1));
  EXPECT_FALSE(
      raw("52434332"
          "0100"
          "0000"
          "00000001"
          "0001"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0101"
          "0000"
          "00000001"
          "0001"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000000"
          "0001"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0001"
          "00000001"
          "0001"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0000"
          "0002",
          2));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "000a"
          "03c0",
          960));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0001"
          "0000",
          0));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0001"
          "03c1",
          961));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0000"
          "00000001"
          "0002"
          "0002",
          2));
  EXPECT_TRUE(
      raw("52434331"
          "0100"
          "0001"
          "00000001"
          "0002"
          "0002",
          2));
  EXPECT_TRUE(
      raw("52434331"
          "0100"
          "0008"
          "00000001"
          "0009"
          "0200",
          512));
  EXPECT_FALSE(
      raw("52434331"
          "0100"
          "0008"
          "00000001"
          "0009"
          "0201",
          513));
  EXPECT_FALSE(
      raw("52434331"
          "0300"
          "0000"
          "00000001"
          "0001"
          "0002",
          2));
}

TEST_F(TransportFrameControl, DecodesOnlyWellFormedAckRecords) {
  EXPECT_TRUE(
      raw("52434331"
          "0200"
          "0000"
          "00000001"
          "0000"
          "0000",
          0));
  EXPECT_FALSE(
      raw("52434331"
          "0200"
          "0001"
          "00000001"
          "0000"
          "0000",
          0));
  EXPECT_FALSE(
      raw("52434331"
          "0200"
          "0000"
          "00000001"
          "0001"
          "0000",
          0));
  EXPECT_FALSE(
      raw("52434331"
          "0200"
          "0000"
          "00000001"
          "0000"
          "0001",
          1));
  EXPECT_FALSE(TransportFrameCodec::decode_control(
      test_support::Bytes::unhex("524343310200000000000001000000")));
}

}  // namespace
}  // namespace net_kakusan_test
