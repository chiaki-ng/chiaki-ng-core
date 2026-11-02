#include "src/core/net/protocol/takion/control_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/codec_rig.hpp"
#include "tests/unit/core/net/protocol/takion/control_codec_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class TakionControlCodecLimits : public TakionControlCodecTest {};

TEST_F(TakionControlCodecLimits, EncodeEveryMessageAtExactlyItsLimit) {
  const takion::ControlMessage messages[]{
      takion::Heartbeat{},
      takion::Disconnect{"bye", full_event()},
      full_big(),
      full_bang(),
      full_stream_info(),
      takion::StreamInfoAcknowledgement{},
      takion::Senkusha{takion::SenkushaEchoCommand{false}},
      takion::Senkusha{full_mtu_command()},
      takion::Senkusha{full_bandwidth_command()},
      takion::Senkusha{full_client_mtu_command()},
      full_connection_quality(),
      takion::CorruptFrame{1, 2},
      takion::ControllerConnection{1, true,
                                   takion::ControllerConnectionType::dualsense},
      takion::IdrRequest{},
      takion::ProtocolRequest{{7, 12, 15, 18, 20}},
      takion::ProtocolRequestAck{20},
      takion::AudioState{
          takion::AudioStateType::flags,
          TakionCodecRig::bytes({0x03, 0xff, 0, 0, 0, 1, 0, 0, 0})},
  };

  for (const auto& message : messages) {
    const auto encoded = codec.encode(message);
    ASSERT_TRUE(encoded);
    const takion::ControlCodec exact{encoded.value().size()};
    const auto exact_encoded = exact.encode(message);
    ASSERT_TRUE(exact_encoded);
    EXPECT_TRUE(std::ranges::equal(exact_encoded.value().bytes(),
                                   encoded.value().bytes()));
    const takion::ControlCodec too_small{encoded.value().size() - 1};
    const auto rejected = too_small.encode(message);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().reason,
              takion::ControlCodecErrorReason::message_too_large);
    EXPECT_EQ(rejected.error().limit, encoded.value().size() - 1);
  }
}

TEST_F(TakionControlCodecLimits, RefuseToDecodeAMessageOverTheLimit) {
  const takion::ControlCodec small{4};
  const auto oversized_decode =
      small.decode(TakionCodecRig::bytes({0x08, 0x03, 0xa0, 0x06, 0x01}));
  ASSERT_FALSE(oversized_decode);
  EXPECT_EQ(oversized_decode.error(),
            (takion::ControlCodecError{
                takion::ControlCodecErrorReason::message_too_large, 5, 4}));
}

TEST_F(TakionControlCodecLimits, RefuseToEncodeAnOversizedString) {
  takion::Disconnect oversized_string;
  oversized_string.reason.assign(takion::default_control_message_size_limit + 1,
                                 'x');
  const auto oversized_string_result =
      codec.encode(takion::ControlMessage{oversized_string});
  ASSERT_FALSE(oversized_string_result);
  EXPECT_EQ(oversized_string_result.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

TEST_F(TakionControlCodecLimits, RefuseToEncodeOversizedBytes) {
  takion::Big oversized_bytes;
  oversized_bytes.client_version = 20;
  oversized_bytes.encrypted_key.assign(
      takion::default_control_message_size_limit + 1, std::byte{0x55});
  const auto oversized_bytes_result =
      codec.encode(takion::ControlMessage{oversized_bytes.clone()});
  ASSERT_FALSE(oversized_bytes_result);
  EXPECT_EQ(oversized_bytes_result.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

TEST_F(TakionControlCodecLimits, RefuseToEncodeAnOversizedCollection) {
  const takion::ControlCodec small{4};
  takion::StreamInfo oversized_collection;
  oversized_collection.audio_header = TakionCodecRig::bytes({});
  oversized_collection.resolutions.assign(5, takion::StreamInfoResolution{});
  const auto oversized_collection_result =
      small.encode(takion::ControlMessage{oversized_collection});
  ASSERT_FALSE(oversized_collection_result);
  EXPECT_EQ(oversized_collection_result.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

TEST_F(TakionControlCodecLimits, RefuseToEncodeAnOversizedRepeatedField) {
  const takion::ControlCodec small{4};
  takion::ProtocolRequest oversized_repeated;
  oversized_repeated.supported_versions.assign(5, 20);
  const auto oversized_repeated_result =
      small.encode(takion::ControlMessage{oversized_repeated});
  ASSERT_FALSE(oversized_repeated_result);
  EXPECT_EQ(oversized_repeated_result.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

TEST_F(TakionControlCodecLimits, CapTheLimitAtWhatTheBackendSupports) {
  const takion::ControlCodec backend_capped{
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1};
  EXPECT_EQ(backend_capped.effective_message_size_limit(),
            static_cast<std::size_t>(std::numeric_limits<int>::max()));
  const auto capped_heartbeat =
      backend_capped.encode(takion::ControlMessage{takion::Heartbeat{}});
  EXPECT_TRUE(capped_heartbeat);
}

TEST_F(TakionControlCodecLimits, BoundTheAudioProfileHeadersOfAChannel) {
  takion::StreamInfo oversized;
  oversized.audio_channels.push_back(takion::StreamInfoAudioChannel{
      0, ByteVector{}, std::nullopt, std::vector<ByteVector>(5), std::nullopt});
  const auto rejected =
      takion::ControlCodec{4}.encode(takion::ControlMessage{oversized});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().reason,
            takion::ControlCodecErrorReason::message_too_large);
}

}  // namespace
}  // namespace takion_test
