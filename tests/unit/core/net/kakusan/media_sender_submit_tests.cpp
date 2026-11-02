#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/net/kakusan/media_sender_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

static_assert(!std::is_copy_constructible_v<MediaSender>);
static_assert(!std::is_copy_constructible_v<MediaKey>);

class MediaSenderSubmission : public MediaSenderTest {};
class MediaSenderPacing : public MediaSenderTest {};
class MediaSenderLifetime : public MediaSenderTest {};

TEST_F(MediaSenderSubmission, SendsTheVectorFragmentsThenAProbe) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  for (int index = 0; index < 7; ++index)
    submit_ok(sender, MediaKind::audio, 1, 0);
  EXPECT_EQ(sender.next_packet_sequence(), 7U);
  const std::uint64_t start = media_queue_age_limit_us + 1;
  const auto submitted =
      sender.submit(MediaKind::video, (std::uint64_t{1} << 32U) + 32,
                    vectors.original(), start);
  EXPECT_TRUE(submitted);
  EXPECT_EQ(sender.queued_packets(), 1U);
  EXPECT_EQ(sender.next_packet_sequence(), 8U);
  EXPECT_FALSE(sender.take_idr_request());

  const auto& expected = vectors.fragments();
  const auto fragments = poll_ok(sender, start);
  ASSERT_EQ(fragments.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index)
    EXPECT_EQ(test_support::Bytes::hex(fragments[index]),
              test_support::kakusan::Fixtures::string_of(expected[index]));
  EXPECT_EQ(sender.queued_packets(), 0U);
  EXPECT_EQ(sender.controller().bytes_in_flight(), 1200U + 232U);
  EXPECT_TRUE(sender.pacer().idle());

  const auto pto = sender.next_deadline_us();
  ASSERT_TRUE(pto);
  EXPECT_EQ(*pto, start + 1024000);
  EXPECT_TRUE(poll_ok(sender, *pto - 1).empty());
  const auto probe = poll_ok(sender, *pto);
  ASSERT_EQ(probe.size(), 1U);
  EXPECT_EQ(test_support::Bytes::hex(probe[0]), vectors.text("probe_hex"));
}

TEST_F(MediaSenderSubmission, RefusesEmptyOversizedAndProbePayloads) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  const auto big = payload(max_source_datagram_size + 1);
  EXPECT_FALSE(sender.submit(MediaKind::video, 0, {}, 0));
  EXPECT_FALSE(sender.submit(MediaKind::video, 0, big, 0));
  EXPECT_FALSE(sender.submit(MediaKind::probe, 0,
                             std::span<const std::byte>{big}.first(10), 0));
  EXPECT_EQ(sender.queued_packets(), 0U);
  EXPECT_EQ(sender.next_packet_sequence(), 0U);
  EXPECT_TRUE(sender.submit(
      MediaKind::video, 0,
      std::span<const std::byte>{big}.first(max_source_datagram_size), 0));
  EXPECT_EQ(sender.queued_packets(), 1U);
  const auto datagrams = poll_ok(sender, 0);
  ASSERT_EQ(datagrams.size(), 1U);
  EXPECT_EQ(datagrams[0].size(), max_datagram_size);
}

TEST_F(MediaSenderPacing, DoesNotAccumulateABurstWhileIdle) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  submit_ok(sender, MediaKind::video, media_fragment_size, 5000000);
  submit_ok(sender, MediaKind::video, media_fragment_size, 5000000);
  const auto first = poll_ok(sender, 5000000);
  EXPECT_EQ(first.size(), 1U);
  const auto due = sender.next_deadline_us();
  ASSERT_TRUE(due);
  EXPECT_GT(*due, 5000000U);
  EXPECT_LE(*due, 5000000U + 2400U);
  EXPECT_EQ(poll_ok(sender, *due).size(), 1U);
}

TEST_F(MediaSenderPacing, ForwardsAsOfferedWithoutWaitingForAcknowledgements) {
  MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
  std::vector<std::uint64_t> times;
  std::uint64_t sent_bytes = 0;
  std::uint64_t next_submit = 0;
  std::uint64_t now = 0;
  while (now < 990000) {
    if (next_submit <= now) {
      submit_ok(sender, MediaKind::video, media_fragment_size, now);
      next_submit += 10000;
    }
    for (const auto& datagram : poll_ok(sender, now)) {
      EXPECT_EQ(header_of(datagram).kind, MediaKind::video);
      times.push_back(now);
      sent_bytes += datagram.size();
    }
    const auto due = sender.next_deadline_us();
    now = std::min(next_submit, due ? std::max(*due, now + 1) : next_submit);
  }
  ASSERT_EQ(times.size(), 99U);
  EXPECT_EQ(sent_bytes, 99U * 1200U);
  for (std::size_t index = 0; index < times.size(); ++index)
    EXPECT_EQ(times[index], index * 10000);
  EXPECT_EQ(sender.controller().bytes_in_flight(), sent_bytes);
  EXPECT_EQ(sender.flow(), VideoFlow::forwarding);
  EXPECT_FALSE(sender.take_idr_request());
  EXPECT_EQ(sender.report().dropped_packets, 0U);
  EXPECT_FALSE(sender.failure());
}

TEST_F(MediaSenderLifetime, WipesItsKeyWhenDestroyed) {
  alignas(MediaSender) std::array<std::byte, sizeof(MediaSender)> storage{};
  const auto holds_key = [&] {
    const std::span<const std::byte> key{vectors.key().data(),
                                         vectors.key().size()};
    return !std::ranges::search(storage, key).empty();
  };
  auto* sender = new (storage.data())
      MediaSender{crypto, vectors.key().clone(), vectors.connection(), 0};
  EXPECT_TRUE(holds_key());
  submit_ok(*sender, MediaKind::video, 5000, 0);
  EXPECT_EQ(poll_ok(*sender, 0).size(), 1U);
  EXPECT_EQ(sender->queued_packets(), 1U);
  EXPECT_NE(sender->queued_bytes(), 0U);
  std::destroy_at(sender);
  EXPECT_FALSE(holds_key());
}

}  // namespace
}  // namespace net_kakusan_test
