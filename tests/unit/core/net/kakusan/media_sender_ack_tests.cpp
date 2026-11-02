#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/media_sender_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class MediaSenderAcks : public MediaSenderTest {
 protected:
  void expect_rejected(MediaSender& sender,
                       const std::vector<std::byte>& record) {
    const auto result = sender.on_ack(record, 10000);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), Error::protocol);
    EXPECT_EQ(sender.controller().bytes_in_flight(), 3U * 176U);
    EXPECT_FALSE(sender.controller().largest_acked());
    EXPECT_FALSE(sender.failure());
  }
};

class MediaSenderProgress : public MediaSenderTest {};

TEST_F(MediaSenderAcks, RejectMalformedRecordsWithoutChangingState) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  send_three_small(sender);
  expect_rejected(sender, ack_record(3, 0, {AckRange{0, 3}}));
  expect_rejected(sender, ack_record(3, 0, {AckRange{3, 3}, AckRange{0, 1}}));
  expect_rejected(sender, ack_record(1000000, 0, {AckRange{1000000, 1000000}}));
  expect_rejected(sender, ack_record(2, 0, {AckRange{2, 2}, AckRange{0, 1}}));
  expect_rejected(sender, ack_record(2, 0, {AckRange{1, 2}, AckRange{0, 1}}));
  expect_rejected(sender, ack_record(2, 0, {AckRange{2, 1}}));
  expect_rejected(sender, ack_record(2, 0, {AckRange{0, 0}, AckRange{2, 2}}));
  expect_rejected(sender, ack_record(1, 0, {AckRange{0, 2}}));
  expect_rejected(sender, ack_record(2, 0, {AckRange{0, 2}}, 1));
  expect_rejected(sender, ack_record(2, 0, {}));
  auto truncated = ack_record(2, 0, {AckRange{0, 2}});
  truncated.pop_back();
  expect_rejected(sender, truncated);
  auto reserved = ack_record(2, 0, {AckRange{0, 2}});
  reserved[18] = std::byte{1};
  expect_rejected(sender, reserved);
  auto prefix = ack_record(2, 0, {AckRange{0, 2}});
  prefix[3] = std::byte{'2'};
  expect_rejected(sender, prefix);
}

TEST_F(MediaSenderAcks, AcceptRepeatedAndPartialRangesUntilAllAreAcknowledged) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  send_three_small(sender);
  EXPECT_TRUE(
      sender.on_ack(ack_record(2, 0, {AckRange{2, 2}, AckRange{0, 0}}), 20000));
  EXPECT_EQ(sender.controller().bytes_in_flight(), 176U);
  EXPECT_TRUE(
      sender.on_ack(ack_record(2, 0, {AckRange{2, 2}, AckRange{0, 0}}), 21000));
  EXPECT_TRUE(sender.on_ack(ack_record(0, 0, {AckRange{0, 0}}), 22000));
  EXPECT_TRUE(sender.on_ack(ack_record(2, 0, {AckRange{0, 2}}), 23000));
  EXPECT_EQ(sender.controller().bytes_in_flight(), 0U);
  const auto still = sender.on_ack(ack_record(3, 0, {AckRange{0, 3}}), 24000);
  ASSERT_FALSE(still);
  EXPECT_EQ(still.error(), Error::protocol);
}

TEST_F(MediaSenderProgress, IsNotRenewedByRepeatedAcks) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (int index = 0; index < 3; ++index)
    submit_ok(sender, MediaKind::audio, 100, 0);
  submit_ok(sender, MediaKind::video, media_fragment_size, 0);
  EXPECT_EQ(poll_ok(sender, 0).size(), 3U);

  EXPECT_TRUE(sender.on_ack(ack_record(2, 0, {AckRange{0, 2}}), 50000));
  EXPECT_EQ(sender.controller().smoothed_rtt_us(), 50000.0);

  const auto big = poll_ok(sender, 50000);
  ASSERT_EQ(big.size(), 1U);
  EXPECT_EQ(header_of(big[0]).datagram_sequence, 3U);
  EXPECT_EQ(sender.progress_deadline_us(), 50000U + media_progress_timeout_us);

  std::uint64_t now = 50000;
  std::size_t probes = 0;
  bool repeated = false;
  while (true) {
    const auto due = sender.next_deadline_us();
    ASSERT_TRUE(due);
    if (!repeated && *due > 2000000) {
      now = 2000000;
      EXPECT_TRUE(sender.on_ack(ack_record(2, 0, {AckRange{0, 2}}), now));
      EXPECT_TRUE(sender.on_ack(
          ack_record(2, 0, {AckRange{2, 2}, AckRange{0, 0}}), now));
      EXPECT_EQ(sender.progress_deadline_us(),
                50000U + media_progress_timeout_us);
      repeated = true;
      continue;
    }
    now = *due;
    Datagrams datagrams;
    const auto polled = sender.poll(now, datagrams);
    if (!polled) {
      EXPECT_EQ(polled.error(), Error::congestion);
      break;
    }
    for (const auto& datagram : datagrams) {
      EXPECT_EQ(header_of(datagram).kind, MediaKind::probe);
      ++probes;
    }
  }
  EXPECT_TRUE(repeated);
  EXPECT_EQ(now, 50000 + media_progress_timeout_us);
  EXPECT_GE(probes, 2U);
}

TEST_F(MediaSenderProgress, EndsTheStreamAfterAnAckBlackout) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (int index = 0; index < 5; ++index)
    submit_ok(sender, MediaKind::audio, 100, 0);
  EXPECT_EQ(poll_ok(sender, 0).size(), 5U);
  EXPECT_EQ(sender.progress_deadline_us(), media_progress_timeout_us);

  std::vector<std::uint64_t> probe_times;
  std::uint64_t now = 0;
  while (true) {
    const auto due = sender.next_deadline_us();
    ASSERT_TRUE(due);
    now = *due;
    Datagrams datagrams;
    const auto polled = sender.poll(now, datagrams);
    if (!polled) {
      EXPECT_EQ(polled.error(), Error::congestion);
      EXPECT_TRUE(datagrams.empty());
      break;
    }
    for (const auto& datagram : datagrams) {
      EXPECT_EQ(datagram.size(), media_probe_size);
      EXPECT_EQ(header_of(datagram).kind, MediaKind::probe);
      EXPECT_EQ(header_of(datagram).datagram_sequence, 5 + probe_times.size());
      probe_times.push_back(now);
    }
  }
  EXPECT_EQ(now, media_progress_timeout_us);
  EXPECT_EQ(probe_times, (std::vector<std::uint64_t>{1024000}));
  EXPECT_EQ(sender.failure(), Error::congestion);
  EXPECT_FALSE(sender.next_deadline_us());
  EXPECT_TRUE(sender.on_ack(ack_record(4, 0, {AckRange{0, 4}}), now + 1));
  Datagrams datagrams;
  EXPECT_FALSE(sender.poll(now + 2, datagrams));
  EXPECT_TRUE(datagrams.empty());
}

TEST_F(MediaSenderProgress, RefusesAnAckArrivingAtTheTimeout) {
  MediaSender boundary{crypto, vectors.key().clone(), vectors.connection(), 0};
  submit_ok(boundary, MediaKind::audio, 100, 0);
  EXPECT_EQ(poll_ok(boundary, 0).size(), 1U);
  EXPECT_EQ(poll_ok(boundary, 1024000).size(), 1U);
  EXPECT_TRUE(poll_ok(boundary, media_progress_timeout_us - 1).empty());
  const auto late = boundary.on_ack(ack_record(0, 0, {AckRange{0, 0}}),
                                    media_progress_timeout_us);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), Error::congestion);
}

}  // namespace
}  // namespace net_kakusan_test
