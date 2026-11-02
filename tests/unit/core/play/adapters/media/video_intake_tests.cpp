#include "libchiaking/detail/media/media_sink.hpp"
#include "src/core/media/video_intake.hpp"
#include "src/core/net/protocol/remote_play/stream/video_assembler.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using Intake = libchiaking::media::VideoIntake;

static_assert(remote_play::VideoAssembler::buffer_padding >=
              libchiaking::media::VideoFrame::tail_padding);

class VideoIntake : public ::testing::Test {
 protected:
  static auto assembled(std::uint16_t frame, std::size_t size)
      -> remote_play::event::EncodedMedia {
    remote_play::event::EncodedMedia unit{remote_play::EncodedMediaKind::video,
                                          {}};
    unit.bytes.reserve(size + remote_play::VideoAssembler::buffer_padding);
    unit.bytes.assign(size, static_cast<std::byte>(frame));
    unit.bytes.back() = std::byte{0x7f};
    unit.frame_index = frame;
    return unit;
  }
};

TEST_F(VideoIntake, TakesTheBufferInsteadOfCopyingIt) {
  auto unit = assembled(9, 4096);
  const auto* bytes = unit.bytes.data();
  const auto before = CountedAllocations::on_this_thread();
  auto frame = Intake::frame(std::move(unit));
  EXPECT_EQ(CountedAllocations::on_this_thread(), before);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->bytes().data(), bytes);
  EXPECT_EQ(frame->size(), 4096);
  EXPECT_TRUE(unit.bytes.empty());
}

TEST_F(VideoIntake, LeavesAudioWithItsOwner) {
  remote_play::event::EncodedMedia audio{remote_play::EncodedMediaKind::audio,
                                         {std::byte{1}, std::byte{2}}};
  EXPECT_FALSE(Intake::frame(std::move(audio)));
  EXPECT_EQ(audio.bytes.size(), 2);
}

}  // namespace
}  // namespace play_test
