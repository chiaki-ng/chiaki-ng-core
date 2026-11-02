#include "tests/unit/core/net/protocol/remote_play/stream/video_bitstream_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <tuple>
#include <vector>

#ifdef CHIAKI_COMPARE_REFERENCE
#include <chiaki/bitstream.h>
extern "C" void chiaki_log(ChiakiLog*, ChiakiLogLevel, const char*, ...) {}
#endif

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class VideoBitstreamParsing
    : public VideoBitstreamRig,
      public ::testing::WithParamInterface<std::tuple<bool, unsigned>> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(decoder.header(parameters));
#ifdef CHIAKI_COMPARE_REFERENCE
    chiaki_bitstream_init(&original, nullptr,
                          hevc ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264);
    ASSERT_TRUE(chiaki_bitstream_header(
        &original, reinterpret_cast<uint8_t*>(parameters.data()),
        static_cast<unsigned>(parameters.size())));
#endif
  }

  const bool hevc = std::get<0>(GetParam());
  const unsigned frame_bits = std::get<1>(GetParam());
  remote_play::VideoBitstream decoder{hevc};
  std::vector<Byte> parameters = header(hevc, frame_bits);
  std::vector<Byte> intra = frame(hevc, frame_bits, 0, true);
#ifdef CHIAKI_COMPARE_REFERENCE
  ChiakiBitstream original{};
#endif
};

TEST_P(VideoBitstreamParsing, ClassifiesAnIntraSlice) {
  const auto parsed = decoder.slice(intra);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->type, remote_play::VideoSliceType::intra);
}

TEST_P(VideoBitstreamParsing, ReadsTheReferenceOfAPredictedSlice) {
  for (unsigned reference = 0; reference < 16; ++reference) {
    auto bytes = frame(hevc, frame_bits, reference);
    const auto parsed = decoder.slice(bytes);
    ASSERT_TRUE(parsed) << "reference " << reference;
    EXPECT_EQ(parsed->type, remote_play::VideoSliceType::predicted)
        << "reference " << reference;
    EXPECT_EQ(parsed->reference, reference);
#ifdef CHIAKI_COMPARE_REFERENCE
    ChiakiBitstreamSlice original_slice{};
    ASSERT_TRUE(chiaki_bitstream_slice(
        &original, reinterpret_cast<uint8_t*>(bytes.data()),
        static_cast<unsigned>(bytes.size()), &original_slice));
    EXPECT_EQ(original_slice.slice_type, CHIAKI_BITSTREAM_SLICE_P);
    EXPECT_EQ(original_slice.reference_frame, parsed->reference);
#endif
  }
}

TEST_P(VideoBitstreamParsing, RepointsHevcSlicesAndLeavesAvcSlicesUntouched) {
  for (unsigned reference = 0; reference < 16; ++reference) {
    const auto bytes = frame(hevc, frame_bits, reference);
    for (unsigned replacement = reference; replacement < 16; ++replacement) {
      auto changed = bytes;
      EXPECT_EQ(decoder.set_reference(changed, replacement), hevc)
          << "reference " << reference << " replacement " << replacement;
      if (hevc) {
        const auto parsed = decoder.slice(changed);
        ASSERT_TRUE(parsed)
            << "reference " << reference << " replacement " << replacement;
        EXPECT_EQ(parsed->reference, replacement);
      } else {
        EXPECT_EQ(changed, bytes)
            << "reference " << reference << " replacement " << replacement;
      }
#ifdef CHIAKI_COMPARE_REFERENCE
      auto original_bytes = bytes;
      EXPECT_EQ(
          chiaki_bitstream_slice_set_reference_frame(
              &original, reinterpret_cast<uint8_t*>(original_bytes.data()),
              static_cast<unsigned>(original_bytes.size()), replacement),
          hevc);
      EXPECT_EQ(original_bytes, changed);
#endif
    }
  }
}

TEST_P(VideoBitstreamParsing, RefusesAReferenceBeyondTheSixteenthFrame) {
  for (unsigned reference = 0; reference < 16; ++reference) {
    auto bytes = frame(hevc, frame_bits, reference);
    const auto unchanged = bytes;
    EXPECT_FALSE(decoder.set_reference(bytes, 16)) << "reference " << reference;
    EXPECT_EQ(bytes, unchanged) << "reference " << reference;
  }
}

TEST_P(VideoBitstreamParsing, RefusesTruncatedSlicesAndHeaders) {
  for (std::size_t length = 0; length < 5; ++length) {
    EXPECT_FALSE(decoder.slice(std::span<const Byte>{intra}.first(length)))
        << "length " << length;
    EXPECT_FALSE(
        decoder.header(std::span<const Byte>{parameters}.first(length)))
        << "length " << length;
  }
}

TEST_P(VideoBitstreamParsing, RefusesASliceOfZeros) {
  std::vector<Byte> malicious(256, Byte{0});
  malicious[3] = Byte{1};
  malicious[4] = hevc ? Byte{2} : Byte{0x61};
  EXPECT_FALSE(decoder.slice(malicious));
}

INSTANTIATE_TEST_SUITE_P(CodecsAndFrameNumberWidths, VideoBitstreamParsing,
                         ::testing::Combine(::testing::Bool(),
                                            ::testing::Values(4U, 8U, 16U)));

class VideoDependencyTracking : public VideoBitstreamRig,
                                public ::testing::WithParamInterface<bool> {
 protected:
  void accept_the_first_intra_frame() {
    history.header(header(hevc, 8));
    EXPECT_FALSE(history.accept(1, predicted));
    ASSERT_TRUE(history.accept(65535, intra));
    ASSERT_TRUE(history.accept(0, predicted));
  }

  void lose_the_previous_frame() {
    ASSERT_NO_FATAL_FAILURE(accept_the_first_intra_frame());
    history.invalidate(0, 0);
    const auto repaired = history.accept(1, missing);
    EXPECT_EQ(repaired.accepted, hevc);
    EXPECT_EQ(repaired.repaired, hevc);
  }

  void ask_for_a_keyframe_after_an_unrepairable_loss() {
    ASSERT_NO_FATAL_FAILURE(lose_the_previous_frame());
    history.invalidate(65535, 1);
    EXPECT_FALSE(history.accept(2, next));
    EXPECT_TRUE(history.request_idr());
    EXPECT_FALSE(history.request_idr());
    EXPECT_FALSE(history.accept(3, next));
    ASSERT_TRUE(history.accept(4, intra));
    const auto plain = history.accept(5, next);
    EXPECT_TRUE(plain.accepted);
    EXPECT_FALSE(plain.repaired);
  }

  const bool hevc = GetParam();
  remote_play::VideoDependencies history{hevc};
  std::vector<Byte> predicted = frame(hevc, 8, 0);
  std::vector<Byte> intra = frame(hevc, 8, 0, true);
  std::vector<Byte> missing = frame(hevc, 8, 0);
  std::vector<Byte> next = frame(hevc, 8, 0);
};

TEST_P(VideoDependencyTracking,
       RefusesPredictedFramesBeforeTheFirstIntraFrame) {
  ASSERT_NO_FATAL_FAILURE(accept_the_first_intra_frame());
}

TEST_P(VideoDependencyTracking, RepairsALostReferenceOnlyInHevc) {
  ASSERT_NO_FATAL_FAILURE(lose_the_previous_frame());
  if (hevc) {
    remote_play::VideoBitstream parser{true};
    ASSERT_TRUE(parser.header(header(true, 8)));
    const auto parsed = parser.slice(missing);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->reference, 1U);
  }
}

TEST_P(VideoDependencyTracking, AsksForOneKeyframeAfterAnUnrepairableLoss) {
  ASSERT_NO_FATAL_FAILURE(ask_for_a_keyframe_after_an_unrepairable_loss());
}

TEST_P(VideoDependencyTracking, RefusesAFrameWhoseWholeHistoryWasLost) {
  ASSERT_NO_FATAL_FAILURE(ask_for_a_keyframe_after_an_unrepairable_loss());
  EXPECT_TRUE(history.request_idr());
  ASSERT_TRUE(history.accept(6, intra));
  for (unsigned index = 7; index < 24; ++index)
    EXPECT_TRUE(history.accept(static_cast<std::uint16_t>(index), next))
        << "frame " << index;
  history.invalidate(8, 23);
  EXPECT_FALSE(history.accept(24, next));
}

INSTANTIATE_TEST_SUITE_P(AvcAndHevc, VideoDependencyTracking,
                         ::testing::Bool());

}  // namespace
}  // namespace remote_play_test
