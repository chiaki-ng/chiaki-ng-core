#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/play/adapters/session/video_intake_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace play_test {

namespace {

using namespace std::chrono_literals;
using api_test::stream_info;
using api_test::video_media;
using libchiaking::media::VideoLossRange;
using libchiaking::media::VideoVerdict;
using test_support::LiveProviders;

class VideoIntakeLoss : public VideoIntakeTest {
 protected:
  static void reject_and_drop(Intake& intake) {
    const auto last = intake.admit(frame(65535));
    ASSERT_TRUE(last);
    EXPECT_TRUE(intake.settle(last.value(), {VideoVerdict::recover, -5}));
    const auto wrapped = intake.admit(frame(0));
    ASSERT_TRUE(wrapped);
    EXPECT_TRUE(intake.settle(wrapped.value(), {VideoVerdict::recover, -6}));
    EXPECT_TRUE(intake.take_loss_report());
    intake.rejected(10, -1);
    intake.rejected(8, -1);
    intake.rejected(12, -1);
    EXPECT_TRUE(intake.take_loss_report());
    const auto dropped = intake.admit(frame(1));
    ASSERT_TRUE(dropped);
    EXPECT_TRUE(intake.settle(dropped.value(), {VideoVerdict::dropped, 0}));
  }

  static auto covers(VideoLossRange range, std::uint16_t index) -> bool {
    return static_cast<std::uint16_t>(index - range.first) <=
           static_cast<std::uint16_t>(range.last - range.first);
  }

  static auto reported(const std::vector<VideoLossRange>& reports,
                       std::uint16_t index) -> bool {
    return std::ranges::any_of(reports, [index](VideoLossRange range) {
      return covers(range, index);
    });
  }
};

TEST_F(VideoIntakeLoss, StartsWithNothingToReport) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  EXPECT_FALSE(intake.idr_due());
  EXPECT_FALSE(intake.take_loss_report());
  EXPECT_FALSE(intake.work_pending());
}

TEST_F(VideoIntakeLoss, JoinsRejectionsIntoOneRangeAcrossTheWrap) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  const auto last = intake.admit(frame(65535));
  ASSERT_TRUE(last);
  EXPECT_TRUE(intake.settle(last.value(), {VideoVerdict::recover, -5}));
  EXPECT_FALSE(intake.idr_due());
  EXPECT_TRUE(intake.work_pending());
  const auto wrapped = intake.admit(frame(0));
  ASSERT_TRUE(wrapped);
  EXPECT_TRUE(intake.settle(wrapped.value(), {VideoVerdict::recover, -6}));
  const auto range = intake.take_loss_report();
  ASSERT_TRUE(range);
  EXPECT_EQ(range->first, 65535);
  EXPECT_EQ(range->last, 0);
  EXPECT_FALSE(intake.take_loss_report());
  EXPECT_EQ(intake.statistics().decoder_rejections, 2U);
  EXPECT_EQ(intake.statistics().last_decoder_error, -6);
}

TEST_F(VideoIntakeLoss, JoinsOutOfOrderRejectionsIntoOneRange) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  intake.rejected(10, -1);
  intake.rejected(8, -1);
  intake.rejected(12, -1);
  const auto range = intake.take_loss_report();
  ASSERT_TRUE(range);
  EXPECT_EQ(*range, (VideoLossRange{8, 12}));
}

TEST_F(VideoIntakeLoss, ReportsADroppedFrameAndAsksForAKeyframe) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  const auto dropped = intake.admit(frame(1));
  ASSERT_TRUE(dropped);
  EXPECT_TRUE(intake.settle(dropped.value(), {VideoVerdict::dropped, 0}));
  EXPECT_TRUE(intake.work_pending());
  const auto range = intake.take_loss_report();
  ASSERT_TRUE(range);
  EXPECT_EQ(*range, (VideoLossRange{1, 1}));
  EXPECT_TRUE(intake.idr_due());
  EXPECT_FALSE(intake.idr_due());
  const auto statistics = intake.statistics();
  EXPECT_EQ(statistics.lost, 1U);
  EXPECT_EQ(statistics.decoder_rejections, 0U);
  EXPECT_EQ(statistics.keyframe_requests, 1U);
  EXPECT_EQ(intake.presented().frames_lost, 1);
}

TEST_F(VideoIntakeLoss, AddsUpWhenThreadsShareIt) {
  constexpr std::uint16_t frames = 4000;
  constexpr std::uint16_t rejections = 3000;
  constexpr std::uint16_t drop_every = 4;
  Intake intake{clock()};
  ASSERT_TRUE(intake.configure(stream_info(1)));
  std::vector<VideoLossRange> reports;
  std::atomic<bool> finished{};
  std::int64_t presented_lost = 0;
  auto loop = LiveProviders::spawn("video loss loop", [&] {
    while (!finished.load(std::memory_order_acquire)) {
      if (const auto report = intake.take_loss_report()) {
        reports.push_back(*report);
      }
      static_cast<void>(intake.idr_due());
    }
  });
  auto decoder = LiveProviders::spawn("video loss decoder", [&] {
    for (std::uint16_t index = 0; index < rejections; ++index) {
      intake.rejected(index, -1);
      presented_lost += intake.presented().frames_lost;
    }
  });
  std::uint64_t drops = 0;
  std::vector<std::uint16_t> dropped;
  for (std::uint16_t index = 0; index < frames; ++index) {
    const auto unit = intake.admit(frame(index));
    if (!unit) {
      ADD_FAILURE() << "frame " << index << " was refused";
      continue;
    }
    const bool drop = index % drop_every == 0;
    if (drop) {
      ++drops;
      dropped.push_back(index);
    }
    EXPECT_TRUE(intake.settle(
        unit.value(),
        {drop ? VideoVerdict::dropped : VideoVerdict::accepted, 0}));
  }
  EXPECT_TRUE(decoder.join());
  finished.store(true, std::memory_order_release);
  EXPECT_TRUE(loop.join());
  if (const auto report = intake.take_loss_report()) {
    reports.push_back(*report);
  }
  presented_lost += intake.presented().frames_lost;
  const auto statistics = intake.statistics();
  EXPECT_EQ(statistics.units, frames);
  EXPECT_EQ(statistics.decoder_rejections, rejections);
  EXPECT_EQ(statistics.lost, drops);
  EXPECT_EQ(presented_lost, static_cast<std::int64_t>(drops));
  for (std::uint16_t index = 0; index < rejections; ++index) {
    EXPECT_TRUE(reported(reports, index)) << "rejected frame " << index;
  }
  for (const auto index : dropped) {
    EXPECT_TRUE(reported(reports, index)) << "dropped frame " << index;
  }
  EXPECT_FALSE(intake.take_loss_report());
}

TEST_F(VideoIntakeLoss, SpacesKeyframeRequestsASecondApart) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  reject_and_drop(intake);
  EXPECT_TRUE(intake.idr_due());
  EXPECT_FALSE(intake.idr_due());
  clock().advance(999ms);
  EXPECT_FALSE(intake.idr_due());
  clock().advance(1ms);
  EXPECT_TRUE(intake.idr_due());
  EXPECT_EQ(intake.statistics().keyframe_requests, 2U);

  clock().advance(5000ms);
  static_cast<void>(intake.presented());
  EXPECT_FALSE(intake.idr_due());
  intake.rejected(20, -1);
  EXPECT_FALSE(intake.idr_due());

  intake.request_idr();
  EXPECT_TRUE(intake.work_pending());
  EXPECT_TRUE(intake.idr_due());
  EXPECT_FALSE(intake.idr_due());
}

TEST_F(VideoIntakeLoss, CountsAnUnansweredFecFailureAsLoss) {
  Intake intake{clock()};
  intake.fec_failed(false);
  EXPECT_TRUE(intake.idr_due());
  intake.fec_failed(false);
  EXPECT_FALSE(intake.idr_due());
  clock().advance(1000ms);
  EXPECT_TRUE(intake.idr_due());
  EXPECT_FALSE(intake.idr_due());
}

TEST_F(VideoIntakeLoss, WaitsASecondAfterAnAnsweredFecFailure) {
  Intake answered{clock()};
  answered.fec_failed(true);
  answered.rejected(1, -1);
  answered.rejected(2, -1);
  EXPECT_FALSE(answered.idr_due());
  clock().advance(1000ms);
  EXPECT_TRUE(answered.idr_due());
}

TEST_F(VideoIntakeLoss, HandsStreamLossToTheNextPresentedFrame) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  auto lossy = video_media(7);
  lossy.frames_lost = 3;
  lossy.frame_recovered = true;
  EXPECT_TRUE(intake.admit(frame(std::move(lossy))));
  EXPECT_TRUE(intake.admit(frame(8)));
  const auto loss = intake.presented();
  EXPECT_EQ(loss.frames_lost, 3U);
  EXPECT_TRUE(loss.recovered);
  const auto none = intake.presented();
  EXPECT_EQ(none.frames_lost, 0U);
  EXPECT_FALSE(none.recovered);
  const auto statistics = intake.statistics();
  EXPECT_EQ(statistics.units, 2U);
  EXPECT_EQ(statistics.bytes, 6U);
  EXPECT_EQ(statistics.lost, 3U);
  EXPECT_EQ(statistics.recovered, 1U);
}

TEST_F(VideoIntakeLoss, ResumesOutputOnAPresentedVerdictAlone) {
  Intake intake{clock()};
  EXPECT_TRUE(intake.configure(stream_info(1)));
  auto lossy = video_media(1);
  lossy.frames_lost = 3;
  const auto first = intake.admit(frame(std::move(lossy)));
  ASSERT_TRUE(first);
  EXPECT_TRUE(intake.settle(first.value(), {VideoVerdict::recover, -5}));
  EXPECT_TRUE(intake.take_loss_report());
  EXPECT_FALSE(intake.idr_due());
  const auto second = intake.admit(frame(2));
  ASSERT_TRUE(second);
  EXPECT_TRUE(intake.settle(second.value(), {VideoVerdict::presented, 0}));
  const auto third = intake.admit(frame(3));
  ASSERT_TRUE(third);
  EXPECT_TRUE(intake.settle(third.value(), {VideoVerdict::recover, -5}));
  EXPECT_FALSE(intake.idr_due());
  const auto loss = intake.presented();
  EXPECT_EQ(loss.frames_lost, 0U);
  EXPECT_FALSE(loss.recovered);
  const auto fourth = intake.admit(frame(4));
  ASSERT_TRUE(fourth);
  EXPECT_TRUE(intake.settle(fourth.value(), {VideoVerdict::recover, -5}));
  const auto fifth = intake.admit(frame(5));
  ASSERT_TRUE(fifth);
  EXPECT_TRUE(intake.settle(fifth.value(), {VideoVerdict::recover, -5}));
  EXPECT_TRUE(intake.idr_due());
  EXPECT_EQ(intake.statistics().decoder_rejections, 4U);
}

}  // namespace
}  // namespace play_test
