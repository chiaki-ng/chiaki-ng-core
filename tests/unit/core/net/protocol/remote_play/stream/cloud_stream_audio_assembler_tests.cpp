#include "tests/unit/core/net/protocol/remote_play/stream/cloud_stream_media_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

using Rig = CloudStreamMediaRig;

class CloudStreamAudioUnit : public ::testing::Test {
 protected:
  static auto validate(std::uint16_t frame, std::uint16_t index,
                       std::uint16_t total, std::uint16_t fec,
                       std::vector<std::byte> data) {
    return remote_play::CloudStreamAudioAssembler::validate_unit(
        Rig::counted_audio_unit(frame, index, total, fec, std::move(data)));
  }
};

TEST_F(CloudStreamAudioUnit, AcceptsWholeSourceAndParityPayloads) {
  const auto source = validate(9, 2, 8, 3, Rig::bytes({0, 1, 2, 3, 4}));
  ASSERT_TRUE(source);
  EXPECT_EQ(source.value().frame_index, 9U);
  EXPECT_EQ(source.value().unit_index, 2U);
  EXPECT_EQ(source.value().source_count, 5U);
  EXPECT_EQ(source.value().fec_count, 3U);
  EXPECT_EQ(source.value().data, Rig::bytes({0, 1, 2, 3, 4}));

  const auto parity = validate(10, 7, 8, 3, Rig::bytes({0xaa, 0xbb}));
  ASSERT_TRUE(parity);
  EXPECT_EQ(parity.value().data, Rig::bytes({0xaa, 0xbb}));
  EXPECT_TRUE(validate(0, 0, 1, 0, Rig::bytes({0})));
  EXPECT_TRUE(validate(65535, 255, 256, 255, Rig::bytes({0xbb})));
  EXPECT_TRUE(
      validate(1, 0, 2, 1, std::vector<std::byte>(4096, std::byte{0xcc})));
}

TEST_F(CloudStreamAudioUnit, RejectsInvalidUnits) {
  auto wrong_family = Rig::counted_audio_unit(1, 0, 2, 0, Rig::bytes({1}));
  wrong_family.type = takion::PacketType{0x02};
  EXPECT_EQ(remote_play::CloudStreamAudioAssembler::validate_unit(wrong_family)
                .error(),
            remote_play::CloudStreamAudioUnitError::invalid_family);
  auto wrong_codec = Rig::counted_audio_unit(1, 0, 2, 0, Rig::bytes({1}));
  wrong_codec.codec = 9;
  EXPECT_EQ(remote_play::CloudStreamAudioAssembler::validate_unit(wrong_codec)
                .error(),
            remote_play::CloudStreamAudioUnitError::invalid_codec);
  EXPECT_EQ(validate(1, 0, 0, 0, Rig::bytes({1})).error(),
            remote_play::CloudStreamAudioUnitError::invalid_count);
  EXPECT_EQ(validate(1, 0, 257, 0, Rig::bytes({1})).error(),
            remote_play::CloudStreamAudioUnitError::invalid_count);
  EXPECT_EQ(validate(1, 0, 4, 4, Rig::bytes({1})).error(),
            remote_play::CloudStreamAudioUnitError::invalid_count);
  EXPECT_EQ(validate(1, 0, 2, 3, Rig::bytes({1})).error(),
            remote_play::CloudStreamAudioUnitError::invalid_count);
  EXPECT_EQ(validate(1, 2, 2, 0, Rig::bytes({1})).error(),
            remote_play::CloudStreamAudioUnitError::invalid_unit_index);
  EXPECT_EQ(validate(1, 0, 2, 0, {}).error(),
            remote_play::CloudStreamAudioUnitError::empty_data);
  EXPECT_EQ(
      validate(1, 0, 2, 0, std::vector<std::byte>(4097, std::byte{0})).error(),
      remote_play::CloudStreamAudioUnitError::data_too_large);
}

class CloudStreamAudioAssembly : public ::testing::Test {
 protected:
  auto empty_after(remote_play::CloudStreamAudioUnit unit,
                   std::uint64_t now_us = 0) -> testing::AssertionResult {
    const auto frames = assembler.accept(std::move(unit), now_us);
    if (!frames) return testing::AssertionFailure() << "the unit was refused";
    if (!frames.value().empty())
      return testing::AssertionFailure()
             << frames.value().size() << " frames came out";
    return testing::AssertionSuccess();
  }

  remote_play::CloudStreamAudioAssembler assembler;
};

TEST_F(CloudStreamAudioAssembly, AssemblesOrderedIndividualFrames) {
  EXPECT_TRUE(empty_after({9, 0, 3, 1, Rig::bytes({0, 1})}));
  EXPECT_TRUE(empty_after({11, 2, 3, 1, Rig::bytes({2, 3})}));
  const auto frames = assembler.accept({10, 1, 3, 1, Rig::bytes({4, 5, 6})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 3U);
  EXPECT_EQ(frames.value()[0].frame_index, 9U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({0, 1}));
  EXPECT_EQ(frames.value()[1].frame_index, 10U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({4, 5, 6}));
  EXPECT_EQ(frames.value()[2].frame_index, 11U);
  EXPECT_EQ(frames.value()[2].bytes, Rig::bytes({2, 3}));
  EXPECT_EQ(assembler.accept({10, 1, 3, 1, Rig::bytes({4})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
}

TEST_F(CloudStreamAudioAssembly, HandlesParityFirstAndTheFrameWrap) {
  EXPECT_TRUE(empty_after({1, 2, 2, 1, Rig::bytes({0x8d, 1, 0x8e, 0})}));
  const auto frames =
      assembler.accept({65535, 0, 2, 1, Rig::bytes({1, 2, 3, 4})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 65535U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({1, 2, 3, 4}));
  EXPECT_EQ(frames.value()[1].frame_index, 0U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({5, 6, 7, 8}));
  EXPECT_EQ(assembler.accept({0, 1, 2, 1, Rig::bytes({5, 6, 7, 8})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
  const auto next = assembler.accept({0, 0, 1, 0, Rig::bytes({9})});
  ASSERT_TRUE(next);
  ASSERT_EQ(next.value().size(), 1U);
  EXPECT_EQ(next.value()[0].frame_index, 0U);
  EXPECT_EQ(next.value()[0].bytes, Rig::bytes({9}));
}

TEST_F(CloudStreamAudioAssembly, RejectsDuplicateUnitsAndStaleGenerations) {
  EXPECT_TRUE(empty_after({5, 0, 3, 0, Rig::bytes({0})}));
  EXPECT_TRUE(empty_after({6, 1, 3, 0, Rig::bytes({1})}));
  EXPECT_EQ(assembler.accept({6, 1, 3, 0, Rig::bytes({9})}).error(),
            remote_play::CloudStreamAudioAssemblerError::duplicate_unit);
  auto frames = assembler.accept({7, 2, 2, 1, Rig::bytes({2})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 5U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({0}));
  EXPECT_EQ(frames.value()[1].frame_index, 6U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({1}));
  EXPECT_TRUE(empty_after({8, 2, 3, 0, Rig::bytes({2})}));
  EXPECT_EQ(assembler.accept({7, 2, 3, 0, Rig::bytes({2})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
  EXPECT_TRUE(empty_after({7, 1, 3, 0, Rig::bytes({1})}));
  frames = assembler.accept({6, 0, 3, 0, Rig::bytes({0})});
  ASSERT_TRUE(frames);
  EXPECT_EQ(frames.value().size(), 3U);
  EXPECT_EQ(assembler.accept({5, 1, 2, 0, Rig::bytes({0})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
  EXPECT_EQ(assembler.accept({6, 0, 3, 0, Rig::bytes({0})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
}

TEST_F(CloudStreamAudioAssembly, UnitZeroOfAnOlderGenerationStartsIt) {
  auto frames = assembler.accept({0, 0, 1, 1, Rig::bytes({1})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value()[0].frame_index, 0U);
  frames = assembler.accept({4, 0, 1, 1, Rig::bytes({2})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value()[0].frame_index, 4U);
  frames = assembler.accept({2, 0, 1, 1, Rig::bytes({3})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value()[0].frame_index, 2U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({3}));
  EXPECT_EQ(assembler.accept({2, 1, 1, 1, Rig::bytes({4})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
  EXPECT_TRUE(empty_after({20, 0, 3, 0, Rig::bytes({5})}));
  const auto flushed = assembler.accept({10, 0, 2, 0, Rig::bytes({6})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 1U);
  EXPECT_EQ(flushed.value()[0].frame_index, 20U);
  EXPECT_EQ(flushed.value()[0].bytes, Rig::bytes({5}));
  frames = assembler.accept({11, 1, 2, 0, Rig::bytes({7})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 10U);
  EXPECT_EQ(frames.value()[1].frame_index, 11U);
}

TEST_F(CloudStreamAudioAssembly, ADuplicateUnitZeroFlushesReceivedSources) {
  EXPECT_TRUE(empty_after({7, 0, 3, 0, Rig::bytes({1})}));
  const auto flushed = assembler.accept({7, 0, 3, 0, Rig::bytes({1})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 1U);
  EXPECT_EQ(flushed.value()[0].frame_index, 7U);
  EXPECT_EQ(flushed.value()[0].bytes, Rig::bytes({1}));
  EXPECT_TRUE(empty_after({8, 1, 3, 0, Rig::bytes({2})}));
  const auto frames = assembler.accept({9, 2, 3, 0, Rig::bytes({3})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 3U);
  EXPECT_EQ(frames.value()[0].frame_index, 7U);
  EXPECT_EQ(frames.value()[2].frame_index, 9U);
}

TEST_F(CloudStreamAudioAssembly, ANewUnitZeroFlushesAnIncompleteGeneration) {
  EXPECT_TRUE(empty_after({10, 0, 3, 0, Rig::bytes({1})}));
  EXPECT_TRUE(empty_after({11, 1, 3, 0, Rig::bytes({2})}));
  const auto flushed = assembler.accept({20, 0, 2, 0, Rig::bytes({3})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 10U);
  EXPECT_EQ(flushed.value()[0].bytes, Rig::bytes({1}));
  EXPECT_EQ(flushed.value()[1].frame_index, 11U);
  EXPECT_EQ(flushed.value()[1].bytes, Rig::bytes({2}));
  const auto frames = assembler.accept({21, 1, 2, 0, Rig::bytes({4})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({3}));
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({4}));
}

TEST_F(CloudStreamAudioAssembly,
       ANewerBaseStartsAGenerationWhenUnitZeroIsLost) {
  EXPECT_TRUE(empty_after({11, 1, 3, 0, Rig::bytes({1})}));
  EXPECT_TRUE(empty_after({12, 2, 3, 0, Rig::bytes({2})}));
  auto flushed = assembler.accept({14, 1, 3, 0, Rig::bytes({3})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 11U);
  EXPECT_EQ(flushed.value()[1].frame_index, 12U);
  EXPECT_TRUE(empty_after({15, 2, 3, 0, Rig::bytes({4})}));
  flushed = assembler.accept({16, 0, 3, 0, Rig::bytes({5})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 14U);
  EXPECT_EQ(flushed.value()[1].frame_index, 15U);
  EXPECT_TRUE(empty_after({17, 1, 3, 0, Rig::bytes({6})}));
  const auto frames = assembler.accept({18, 2, 3, 0, Rig::bytes({7})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 3U);
  EXPECT_EQ(frames.value()[0].frame_index, 16U);
  EXPECT_EQ(frames.value()[2].frame_index, 18U);
}

TEST_F(CloudStreamAudioAssembly, ASupersededGenerationEmitsItsSources) {
  EXPECT_TRUE(empty_after({20, 0, 2, 2, Rig::bytes({1, 2, 3, 4})}));
  const auto flushed = assembler.accept({30, 0, 1, 0, Rig::bytes({9})});
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 20U);
  EXPECT_EQ(flushed.value()[0].bytes, Rig::bytes({1, 2, 3, 4}));
  EXPECT_EQ(flushed.value()[1].frame_index, 30U);
  EXPECT_EQ(flushed.value()[1].bytes, Rig::bytes({9}));
}

TEST_F(CloudStreamAudioAssembly, ATimedOutGenerationIsFlushed) {
  EXPECT_TRUE(empty_after({10, 0, 3, 0, Rig::bytes({1})}, 1'000));
  EXPECT_TRUE(empty_after({11, 1, 3, 0, Rig::bytes({2})}, 151'000));
  auto flushed = assembler.accept({12, 2, 3, 0, Rig::bytes({3})}, 151'001);
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 10U);
  EXPECT_EQ(flushed.value()[1].frame_index, 11U);
  flushed = assembler.accept({13, 0, 1, 0, Rig::bytes({4})}, 151'002);
  ASSERT_TRUE(flushed);
  ASSERT_EQ(flushed.value().size(), 2U);
  EXPECT_EQ(flushed.value()[0].frame_index, 12U);
  EXPECT_EQ(flushed.value()[1].frame_index, 13U);
}

TEST_F(CloudStreamAudioAssembly, BoundsTheAudioItHolds) {
  for (std::uint16_t index = 0; index < 64; ++index)
    EXPECT_TRUE(
        empty_after({static_cast<std::uint16_t>(100 + index), index, 200, 0,
                     std::vector<std::byte>(4096, std::byte{0})}))
        << "index " << index;
  EXPECT_EQ(
      assembler
          .accept({164, 64, 200, 0, std::vector<std::byte>(4096, std::byte{0})})
          .error(),
      remote_play::CloudStreamAudioAssemblerError::hold_limit_exceeded);
}

TEST_F(CloudStreamAudioAssembly, RecoversAMissingSource) {
  EXPECT_TRUE(empty_after({20, 0, 2, 1, Rig::bytes({1, 2, 3, 4})}));
  const auto frames =
      assembler.accept({22, 2, 2, 1, Rig::bytes({0x8d, 1, 0x8e, 0})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 20U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({1, 2, 3, 4}));
  EXPECT_EQ(frames.value()[1].frame_index, 21U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({5, 6, 7, 8}));
  EXPECT_EQ(assembler.accept({21, 1, 2, 1, Rig::bytes({5, 6, 7, 8})}).error(),
            remote_play::CloudStreamAudioAssemblerError::stale_generation);
  const auto next = assembler.accept({30, 0, 1, 0, Rig::bytes({9})});
  ASSERT_TRUE(next);
  ASSERT_EQ(next.value().size(), 1U);
  EXPECT_EQ(next.value()[0].bytes, Rig::bytes({9}));
}

TEST_F(CloudStreamAudioAssembly, RecoversAMissingSourceAfterParityFirst) {
  EXPECT_TRUE(empty_after({22, 2, 2, 1, Rig::bytes({0x8d, 1, 0x8e, 0})}));
  const auto frames = assembler.accept({20, 0, 2, 1, Rig::bytes({1, 2, 3, 4})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 20U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({1, 2, 3, 4}));
  EXPECT_EQ(frames.value()[1].frame_index, 21U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({5, 6, 7, 8}));
}

TEST_F(CloudStreamAudioAssembly, RecoversAudioFromTwoParityUnits) {
  EXPECT_TRUE(empty_after({42, 2, 2, 2, Rig::bytes({0x8d, 3, 0x79, 0xf1})}));
  const auto frames =
      assembler.accept({43, 3, 2, 2, Rig::bytes({0x78, 0xf6, 0x8c, 0xf3})});
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 2U);
  EXPECT_EQ(frames.value()[0].frame_index, 40U);
  EXPECT_EQ(frames.value()[0].bytes, Rig::bytes({1, 2, 3, 4}));
  EXPECT_EQ(frames.value()[1].frame_index, 41U);
  EXPECT_EQ(frames.value()[1].bytes, Rig::bytes({5, 6, 7, 8}));
}

}  // namespace
}  // namespace remote_play_test
