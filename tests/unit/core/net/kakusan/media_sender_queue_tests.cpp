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

class MediaSenderQueue : public MediaSenderTest {
 protected:
  void fill_beyond_the_byte_bound(MediaKind kind) {
    MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
    const std::size_t fit = media_queue_bytes / max_source_datagram_size;
    for (std::size_t index = 0; index < fit; ++index)
      submit_ok(sender, kind, max_source_datagram_size, 0);
    EXPECT_EQ(sender.queued_packets(), fit);
    EXPECT_EQ(sender.queued_bytes(), fit * max_source_datagram_size);
    EXPECT_FALSE(sender.take_idr_request());
    submit_ok(sender, kind, max_source_datagram_size, 0);
    EXPECT_EQ(sender.queued_packets(), fit);
    EXPECT_LE(sender.queued_bytes(), media_queue_bytes);
    const auto idr = sender.take_idr_request();
    EXPECT_EQ(idr, kind == MediaKind::video);
    const auto datagrams = poll_ok(sender, 0);
    ASSERT_EQ(datagrams.size(), 1U);
    EXPECT_EQ(header_of(datagrams[0]).packet_sequence, 1U);
    EXPECT_EQ(header_of(datagrams[0]).datagram_sequence, 0U);
    EXPECT_FALSE(sender.failure());
  }
};

TEST_F(MediaSenderQueue, DropsPacketsOlderThanTheAgeLimitAndAsksForAKeyframe) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  submit_ok(sender, MediaKind::video, 3000, 0);
  const auto first = poll_ok(sender, 0);
  ASSERT_EQ(first.size(), 1U);
  EXPECT_EQ(header_of(first[0]).packet_sequence, 0U);
  EXPECT_EQ(header_of(first[0]).fragment_offset, 0U);

  submit_ok(sender, MediaKind::audio, 100, media_queue_age_limit_us);
  EXPECT_EQ(sender.queued_packets(), 2U);
  EXPECT_FALSE(sender.take_idr_request());

  submit_ok(sender, MediaKind::audio, 100, media_queue_age_limit_us + 1);
  EXPECT_EQ(sender.queued_packets(), 2U);
  EXPECT_TRUE(sender.take_idr_request());
  EXPECT_FALSE(sender.take_idr_request());

  const auto rest = poll_ok(sender, media_queue_age_limit_us + 1);
  ASSERT_EQ(rest.size(), 2U);
  for (std::size_t index = 0; index < rest.size(); ++index) {
    const auto header = header_of(rest[index]);
    EXPECT_EQ(header.kind, MediaKind::audio);
    EXPECT_EQ(header.datagram_sequence, index + 1);
    EXPECT_EQ(header.packet_sequence, index + 1);
  }
}

TEST_F(MediaSenderQueue, DropsStaleAudioWithoutAskingForAKeyframe) {
  MediaSender audio{crypto, vectors.key().clone(), vectors.connection(), 0};
  submit_ok(audio, MediaKind::audio, 100, 0);
  EXPECT_TRUE(poll_ok(audio, media_queue_age_limit_us + 1).empty());
  EXPECT_EQ(audio.queued_packets(), 0U);
  EXPECT_FALSE(audio.take_idr_request());
  EXPECT_FALSE(audio.failure());
}

TEST_F(MediaSenderQueue, KeepsVideoWithinTheByteBound) {
  fill_beyond_the_byte_bound(MediaKind::video);
}

TEST_F(MediaSenderQueue, KeepsAudioWithinTheByteBound) {
  fill_beyond_the_byte_bound(MediaKind::audio);
}

TEST_F(MediaSenderQueue, KeepsTheNewestPacketsWithinThePacketBound) {
  MediaSender count{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (std::size_t index = 0; index < media_queue_packets + 5; ++index)
    submit_ok(count, MediaKind::audio, 1, 0);
  EXPECT_EQ(count.queued_packets(), media_queue_packets);
  EXPECT_EQ(count.queued_bytes(), media_queue_packets);
  EXPECT_FALSE(count.take_idr_request());
  const auto datagrams = poll_ok(count, 0);
  ASSERT_FALSE(datagrams.empty());
  EXPECT_EQ(header_of(datagrams[0]).packet_sequence, 5U);
}

}  // namespace
}  // namespace net_kakusan_test
