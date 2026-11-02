#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/media_sender_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class MediaSenderCongestion : public MediaSenderTest {
 protected:
  auto drop_in(MediaSender& sender, std::uint64_t window) -> bool {
    submit_ok(sender, MediaKind::video, 500, window * media_drop_window_us);
    Datagrams datagrams;
    const auto polled =
        sender.poll(window * media_drop_window_us + 300000, datagrams);
    EXPECT_TRUE(datagrams.empty());
    EXPECT_TRUE(sender.take_idr_request());
    return static_cast<bool>(polled);
  }

  static auto lenient() -> MediaTuning {
    MediaTuning tuning;
    tuning.startup_grace_us = 0;
    tuning.progress_timeout_us = 600000000;
    return tuning;
  }

  static auto second(MediaSender& sender, std::uint64_t window,
                     std::size_t stale, std::size_t fresh) -> bool {
    const auto start = window * media_drop_window_us;
    for (std::size_t index = 0; index < stale; ++index)
      submit_ok(sender, MediaKind::video, 100, start);
    Datagrams datagrams;
    for (std::size_t index = 0; index < fresh; ++index) {
      if (!sender.submit(MediaKind::video, 0, payload(100), start + 300000))
        return false;
    }
    return static_cast<bool>(sender.poll(start + 300001, datagrams));
  }
};

class MediaSenderShedding : public MediaSenderTest {
 protected:
  auto send_audio(MediaSender& sender, std::uint64_t now) -> std::uint64_t {
    submit_ok(sender, MediaKind::audio, 100, now);
    const auto datagrams = poll_ok(sender, now);
    EXPECT_EQ(datagrams.size(), 1U);
    if (datagrams.empty()) return 0;
    EXPECT_EQ(header_of(datagrams[0]).kind, MediaKind::audio);
    return header_of(datagrams[0]).datagram_sequence;
  }

  void round(MediaSender& sender, std::uint64_t& now,
             std::uint64_t round_trip) {
    const auto sequence = send_audio(sender, now);
    now += round_trip;
    EXPECT_TRUE(
        sender.on_ack(ack_record(sequence, 0, {AckRange{0, sequence}}), now));
  }
};

class MediaTuningBounds : public MediaSenderTest {};

TEST_F(MediaSenderCongestion, EndsVideoAfterThreeDropWindowsInARow) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (std::uint64_t window = 0; window < 5; ++window)
    EXPECT_TRUE(drop_in(sender, window));
  EXPECT_FALSE(sender.failure());
  EXPECT_EQ(sender.report().dropped_video, 5U);
  EXPECT_EQ(sender.report().stop, MediaStop::none);
  EXPECT_TRUE(drop_in(sender, 5));
  EXPECT_TRUE(drop_in(sender, 7));
  EXPECT_TRUE(drop_in(sender, 8));
  EXPECT_TRUE(drop_in(sender, 9));
  EXPECT_FALSE(sender.failure());
  submit_ok(sender, MediaKind::video, 500, 10 * media_drop_window_us);
  Datagrams none;
  const auto failed = sender.poll(10 * media_drop_window_us, none);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), Error::congestion);
  EXPECT_EQ(sender.failure(), Error::congestion);
  EXPECT_EQ(sender.report().stop, MediaStop::drop_windows);
  EXPECT_FALSE(sender.next_deadline_us());
  Datagrams datagrams;
  const auto again = sender.poll(11000000, datagrams);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), Error::congestion);
  EXPECT_TRUE(datagrams.empty());
  EXPECT_TRUE(sender.submit(MediaKind::video, 0, payload(10), 11000000));
  EXPECT_EQ(sender.queued_packets(), 0U);
}

TEST_F(MediaSenderCongestion, EndsAudioAfterThreeDropWindowsInARow) {
  MediaSender audio{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (std::uint64_t window = 5; window < 7; ++window) {
    submit_ok(audio, MediaKind::audio, 100,
              window * media_drop_window_us + 700000);
    Datagrams out;
    const auto polled =
        audio.poll(window * media_drop_window_us + 1000001, out);
    EXPECT_TRUE(polled);
    EXPECT_TRUE(out.empty());
  }
  EXPECT_FALSE(audio.failure());
  submit_ok(audio, MediaKind::audio, 100, 8000000);
  submit_ok(audio, MediaKind::audio, 100, 8250001);
  EXPECT_FALSE(audio.failure());
  const auto ended = audio.submit(MediaKind::audio, 0, payload(100), 9000000);
  ASSERT_FALSE(ended);
  EXPECT_EQ(ended.error(), Error::congestion);
  EXPECT_EQ(audio.failure(), Error::congestion);
  EXPECT_FALSE(audio.take_idr_request());
}

TEST_F(MediaSenderCongestion, SurvivesAFewDropsEachSecond) {
  MediaSender patchy{crypto, vectors.key().clone(), vectors.connection(), 0,
                     lenient()};
  for (std::uint64_t window = 0; window < 30; ++window)
    EXPECT_TRUE(second(patchy, window, 1, 3));
  EXPECT_FALSE(patchy.failure());
  EXPECT_EQ(patchy.report().stop, MediaStop::none);
  EXPECT_EQ(patchy.report().dropped_video, 30U);
}

TEST_F(MediaSenderCongestion, EndsWhenMostOfEverySecondIsDropped) {
  MediaSender starved{crypto, vectors.key().clone(), vectors.connection(), 0,
                      lenient()};
  EXPECT_TRUE(second(starved, 0, 3, 1));
  EXPECT_TRUE(second(starved, 1, 3, 1));
  EXPECT_TRUE(second(starved, 2, 3, 1));
  EXPECT_FALSE(starved.failure());
  EXPECT_FALSE(second(starved, 3, 3, 1));
  EXPECT_EQ(starved.failure(), Error::congestion);
  EXPECT_EQ(starved.report().stop, MediaStop::drop_windows);
}

TEST_F(MediaSenderCongestion, ForgivesDropWindowsBrokenByAGoodSecond) {
  MediaSender recovering{crypto, vectors.key().clone(), vectors.connection(), 0,
                         lenient()};
  EXPECT_TRUE(second(recovering, 0, 3, 1));
  EXPECT_TRUE(second(recovering, 1, 3, 1));
  EXPECT_TRUE(second(recovering, 2, 1, 3));
  EXPECT_TRUE(second(recovering, 3, 3, 1));
  EXPECT_TRUE(second(recovering, 4, 3, 1));
  EXPECT_FALSE(recovering.failure());
}

TEST_F(MediaSenderShedding, ShedsVideoAndKeepsAudioOnASlowPath) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  std::uint64_t now = 0;
  round(sender, now, 50000);
  for (int slow = 0; slow < 6; ++slow) round(sender, now, 400000);
  EXPECT_EQ(sender.flow(), VideoFlow::forwarding);

  const auto last = send_audio(sender, now);
  submit_ok(sender, MediaKind::video, 5000, now);
  EXPECT_EQ(sender.queued_packets(), 1U);
  now += 400000;
  EXPECT_TRUE(sender.on_ack(ack_record(last, 0, {AckRange{0, last}}), now));
  EXPECT_EQ(sender.flow(), VideoFlow::shedding);
  EXPECT_EQ(sender.queued_packets(), 0U);
  EXPECT_EQ(sender.queued_bytes(), 0U);
  EXPECT_EQ(sender.report().dropped_video, 1U);

  submit_ok(sender, MediaKind::video, 5000, now);
  EXPECT_EQ(sender.queued_packets(), 0U);
  EXPECT_EQ(sender.report().dropped_video, 2U);
  EXPECT_EQ(sender.next_packet_sequence(), 10U);
  EXPECT_FALSE(sender.take_idr_request());
  for (int quick = 0; quick < 5; ++quick) round(sender, now, 50000);
  EXPECT_EQ(sender.flow(), VideoFlow::shedding);
  EXPECT_FALSE(sender.take_idr_request());

  round(sender, now, 50000);
  EXPECT_EQ(sender.flow(), VideoFlow::forwarding);
  EXPECT_TRUE(sender.take_idr_request());
  submit_ok(sender, MediaKind::video, 1000, now);
  const auto resumed = poll_ok(sender, now);
  ASSERT_EQ(resumed.size(), 1U);
  EXPECT_EQ(header_of(resumed[0]).kind, MediaKind::video);
  EXPECT_EQ(sender.report().dropped_video, 2U);
  EXPECT_FALSE(sender.failure());
}

TEST_F(MediaSenderCongestion, FollowsATunedAgeLimitAndDropWindowCount) {
  MediaTuning tuning;
  tuning.queue_age_min_us = 400000;
  tuning.queue_age_limit_us = 400000;
  tuning.startup_grace_us = 0;
  tuning.drop_windows_to_fail = 1;
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0,
                     tuning};
  EXPECT_EQ(sender.queue_age_us(), 400000U);
  for (int index = 0; index < 5; ++index)
    submit_ok(sender, MediaKind::video, 8000, 0);
  EXPECT_FALSE(poll_ok(sender, 0).empty());
  EXPECT_GT(sender.queued_packets(), 0U);
  Datagrams datagrams;
  EXPECT_TRUE(sender.poll(400000, datagrams));
  EXPECT_FALSE(sender.failure());
  EXPECT_TRUE(sender.poll(400001, datagrams));
  EXPECT_FALSE(sender.failure());
  EXPECT_GT(sender.report().dropped_video, 0U);
  submit_ok(sender, MediaKind::video, 8000, media_drop_window_us);
  const auto ended = sender.poll(media_drop_window_us, datagrams);
  ASSERT_FALSE(ended);
  EXPECT_EQ(ended.error(), Error::congestion);
  EXPECT_EQ(sender.report().stop, MediaStop::drop_windows);
}

TEST_F(MediaSenderCongestion, FollowsATunedPacketBound) {
  MediaTuning packets;
  packets.queue_packets = 2;
  MediaSender small{crypto, vectors.key().clone(), vectors.connection(), 0,
                    packets};
  for (int index = 0; index < 5; ++index)
    submit_ok(small, MediaKind::audio, 100, 0);
  EXPECT_EQ(small.queued_packets(), 2U);
}

TEST_F(MediaTuningBounds, RaiseZeroesToTheirFloors) {
  MediaTuning silly;
  silly.queue_age_min_us = 0;
  silly.queue_age_limit_us = 0;
  silly.queue_age_round_trips = 0;
  silly.drop_windows_to_fail = 0;
  silly.progress_timeout_us = 0;
  silly.queue_bytes = 0;
  silly.queue_packets = 0;
  const auto bounded = silly.bounded();
  EXPECT_EQ(bounded.queue_age_min_us, 1000U);
  EXPECT_EQ(bounded.queue_age_limit_us, 1000U);
  EXPECT_EQ(bounded.queue_age_round_trips, 1U);
  EXPECT_EQ(bounded.drop_windows_to_fail, 1U);
  EXPECT_EQ(bounded.progress_timeout_us, 100000U);
  EXPECT_EQ(bounded.queue_packets, 1U);
  EXPECT_EQ(bounded.queue_bytes, max_source_datagram_size);
  EXPECT_EQ(MediaTuning{}.bounded(), MediaTuning{});
}

}  // namespace
}  // namespace net_kakusan_test
