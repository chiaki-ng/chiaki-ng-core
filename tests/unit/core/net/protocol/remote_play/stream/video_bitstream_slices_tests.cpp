#include "tests/unit/core/net/protocol/remote_play/stream/video_bitstream_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class VideoBitstreamSlices : public VideoBitstreamRig {
 protected:
  void SetUp() override {
    ASSERT_TRUE(parser.header(sliced_header(8, false)));
    for (unsigned index = 0; index < 4; ++index) {
      const auto slice = slice_unit(8, 0, index * 60, false);
      four_byte.insert(four_byte.end(), slice.begin(), slice.end());
      three_byte.insert(three_byte.end(), slice.begin() + (index == 0 ? 0 : 1),
                        slice.end());
    }
  }

  remote_play::VideoBitstream parser{true};
  std::vector<Byte> four_byte;
  std::vector<Byte> three_byte;
};

TEST_F(VideoBitstreamSlices, RepointEverySliceOfAFrame) {
  for (const bool dependent : {false, true}) {
    remote_play::VideoBitstream sliced{true};
    ASSERT_TRUE(sliced.header(sliced_header(8, dependent)));
    std::vector<std::vector<Byte>> slices;
    for (unsigned index = 0; index < 4; ++index)
      slices.push_back(
          slice_unit(8, 0, index * 60, dependent, dependent && index == 2));
    std::vector<Byte> whole;
    for (const auto& slice : slices)
      whole.insert(whole.end(), slice.begin(), slice.end());
    for (unsigned replacement = 1; replacement < 16; ++replacement) {
      auto changed = whole;
      ASSERT_TRUE(sliced.set_reference(changed, replacement))
          << "dependent " << dependent << " replacement " << replacement;
      std::size_t offset = 0;
      for (unsigned index = 0; index < 4; ++index) {
        const auto size = slices[index].size();
        std::span<Byte> piece{changed.data() + offset, size};
        if (dependent && index == 2) {
          EXPECT_TRUE(
              std::equal(piece.begin(), piece.end(), slices[index].begin()))
              << "replacement " << replacement;
        } else {
          const auto parsed = sliced.slice(piece);
          ASSERT_TRUE(parsed) << "dependent " << dependent << " replacement "
                              << replacement << " slice " << index;
          EXPECT_EQ(parsed->type, remote_play::VideoSliceType::predicted)
              << "dependent " << dependent << " replacement " << replacement
              << " slice " << index;
          EXPECT_EQ(parsed->reference, replacement)
              << "dependent " << dependent << " slice " << index;
        }
        offset += size;
      }
    }
  }
}

TEST_F(VideoBitstreamSlices, RewriteThreeAndFourByteStartCodesAlike) {
  auto changed_four = four_byte;
  auto changed_three = three_byte;
  ASSERT_TRUE(parser.set_reference(changed_four, 2));
  ASSERT_TRUE(parser.set_reference(changed_three, 2));
  std::vector<Byte> expected_three;
  std::size_t offset = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const auto size = slice_unit(8, 0, index * 60, false).size();
    expected_three.insert(
        expected_three.end(),
        changed_four.begin() +
            static_cast<std::ptrdiff_t>(offset + (index ? 1 : 0)),
        changed_four.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
  }
  EXPECT_EQ(changed_three, expected_three);
}

TEST_F(VideoBitstreamSlices,
       LeaveAFrameUntouchedWhenOneSliceCannotBeRepointed) {
  std::vector<Byte> blocked;
  for (unsigned index = 0; index < 4; ++index) {
    const auto slice = slice_unit(8, 0, index * 60, false, false, index == 2);
    blocked.insert(blocked.end(), slice.begin(), slice.end());
  }
  const auto untouched = blocked;
  EXPECT_FALSE(parser.set_reference(blocked, 3));
  EXPECT_EQ(blocked, untouched);
}

TEST_F(VideoBitstreamSlices, AreAllRepairedAfterALostReference) {
  remote_play::VideoDependencies history{true};
  history.header(sliced_header(8, false));
  auto intra = frame(true, 8, 0, true);
  ASSERT_TRUE(history.accept(10, intra));
  auto lost = four_byte;
  const auto repaired = history.accept(12, lost);
  EXPECT_TRUE(repaired.accepted);
  EXPECT_TRUE(repaired.repaired);
  std::size_t offset = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const auto size = slice_unit(8, 0, index * 60, false).size();
    const auto parsed =
        parser.slice(std::span<Byte>{lost.data() + offset, size});
    ASSERT_TRUE(parsed) << "slice " << index;
    EXPECT_EQ(parsed->reference, 1U) << "slice " << index;
    offset += size;
  }
}

}  // namespace
}  // namespace remote_play_test
