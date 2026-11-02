#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/kakusan/media_sender_rig.hpp"
#include "tests/unit/core/net/kakusan/wire_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

struct LinkScenario {
  double bandwidth{};
  std::uint64_t propagation_us{};
  std::uint64_t loss_ppm{};
  std::uint64_t reorder_ppm{};
  std::uint64_t reorder_extra_us{};
  std::uint64_t ack_loss_ppm{};
  std::size_t bottleneck_bytes{};
  std::size_t frame_bytes{};
  std::size_t keyframe_bytes{};
  std::uint64_t duration_us{};
  std::uint64_t seed{};
};

struct LinkReport {
  std::size_t submitted{};
  std::size_t delivered{};
  std::uint64_t max_latency_us{};
  std::size_t max_queued_bytes{};
  std::uint64_t max_queue_age_us{};
  std::uint64_t max_in_flight{};
  std::size_t datagrams{};
  std::size_t idr_requests{};
  std::size_t keyframes{};
  std::size_t acks{};
  std::optional<Error> failure;
  std::uint64_t failure_us{};
};

class XorShift {
 public:
  explicit XorShift(std::uint64_t seed) noexcept : state_(seed) {}

  auto next() -> std::uint64_t {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  auto chance(std::uint64_t per_million) -> bool {
    return next() % 1000000 < per_million;
  }

 private:
  std::uint64_t state_;
};

class AckingReceiver {
 public:
  AckingReceiver(libchiaking::platform::CryptoProvider& crypto,
                 const WireVectors& vectors,
                 const std::vector<std::uint64_t>& submit_times,
                 const std::vector<std::vector<std::byte>>& sources)
      : crypto_(crypto),
        vectors_(vectors),
        submit_times_(submit_times),
        sources_(sources) {}

  void accept(std::span<const std::byte> datagram, std::uint64_t now,
              LinkReport& report) {
    EXPECT_TRUE(
        TransportFrameCodec::media_matches(datagram, vectors_.connection()));
    EXPECT_TRUE(TransportFrameCodec::authenticate_media(crypto_, vectors_.key(),
                                                        datagram)
                    .value());
    const auto decoded = TransportFrameCodec::decode_media(datagram);
    ASSERT_TRUE(decoded);
    const auto& header = decoded.value().header;
    const auto sequence = static_cast<std::size_t>(header.datagram_sequence);
    if (sequence >= accepted_.size()) accepted_.resize(sequence + 1, false);
    EXPECT_FALSE(accepted_[sequence]);
    accepted_[sequence] = true;
    if (!largest_ || sequence > *largest_) {
      if (largest_ && sequence != *largest_ + 1) immediate_ = true;
      largest_ = sequence;
      largest_arrival_ = now;
    } else {
      immediate_ = true;
    }
    if (pending_ == 0) first_pending_ = now;
    ++pending_;

    if (header.kind == MediaKind::probe) return;
    const auto packet = static_cast<std::size_t>(header.packet_sequence);
    auto& partial = partial_[packet];
    if (partial.bytes.empty()) partial.bytes.resize(header.original_size);
    ASSERT_EQ(partial.bytes.size(), header.original_size);
    std::copy(decoded.value().fragment.begin(), decoded.value().fragment.end(),
              partial.bytes.begin() + header.fragment_offset);
    partial.received += header.fragment_size;
    if (partial.received == partial.bytes.size()) {
      EXPECT_EQ(partial.bytes, sources_[packet]);
      ++report.delivered;
      report.max_latency_us =
          std::max(report.max_latency_us, now - submit_times_[packet]);
      partial_.erase(packet);
    }
    while (!partial_.empty() && partial_.begin()->first + 2048 < packet)
      partial_.erase(partial_.begin());
  }

  auto ack_due(std::uint64_t now) const -> bool {
    return pending_ != 0 &&
           (pending_ >= 2 || immediate_ || now - first_pending_ >= 25000);
  }

  auto make_ack(std::uint64_t now) -> std::vector<std::byte> {
    MediaAck ack;
    ack.largest = *largest_;
    ack.delay_us = static_cast<std::uint32_t>(now - largest_arrival_);
    const std::size_t floor = *largest_ >= 2047 ? *largest_ - 2047 : 0;
    auto cursor = *largest_;
    while (ack.ranges.size() < max_ack_ranges) {
      const auto high = cursor;
      while (cursor > floor && accepted_[cursor - 1]) --cursor;
      ack.ranges.push_back(AckRange{cursor, high});
      while (cursor > floor && !accepted_[cursor - 1]) --cursor;
      if (cursor <= floor) break;
      --cursor;
    }
    pending_ = 0;
    immediate_ = false;
    const auto encoded = TransportFrameCodec::encode_media_ack(ack);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return encoded.value();
  }

 private:
  struct Partial {
    std::vector<std::byte> bytes;
    std::size_t received{};
  };

  libchiaking::platform::CryptoProvider& crypto_;
  const WireVectors& vectors_;
  const std::vector<std::uint64_t>& submit_times_;
  const std::vector<std::vector<std::byte>>& sources_;
  std::vector<bool> accepted_;
  std::map<std::size_t, Partial> partial_;
  std::optional<std::size_t> largest_;
  std::uint64_t largest_arrival_{};
  std::uint64_t first_pending_{};
  std::size_t pending_{};
  bool immediate_{};
};

class MediaSenderSimulation : public MediaSenderTest {
 protected:
  using Scenario = LinkScenario;
  using Report = LinkReport;

  static auto lossy_path() -> Scenario {
    Scenario scenario;
    scenario.propagation_us = 10000;
    scenario.loss_ppm = 3000;
    scenario.reorder_ppm = 1000;
    scenario.reorder_extra_us = 3000;
    scenario.ack_loss_ppm = 10000;
    scenario.bottleneck_bytes = 64 * 1024;
    scenario.frame_bytes = 8000;
    scenario.duration_us = 10000000;
    return scenario;
  }

  static auto long_path(std::uint64_t round_trip_us, std::size_t keyframe_bytes)
      -> Scenario {
    Scenario scenario;
    scenario.bandwidth = 6.25;
    scenario.propagation_us = round_trip_us / 2;
    scenario.bottleneck_bytes = 256 * 1024;
    scenario.frame_bytes = 3000;
    scenario.keyframe_bytes = keyframe_bytes;
    scenario.duration_us = 20000000;
    scenario.seed = 0x9e3779b97f4a7c15ULL;
    return scenario;
  }

  auto simulate(const Scenario& scenario) -> Report {
    constexpr std::uint64_t tick = 100;
    Report report;
    XorShift rng{scenario.seed};
    MediaSender sender{crypto, vectors.key().clone(), vectors.connection(), 0};
    std::vector<std::uint64_t> submit_times;
    std::vector<std::vector<std::byte>> sources;
    AckingReceiver receiver{crypto, vectors, submit_times, sources};
    std::multimap<std::uint64_t, std::vector<std::byte>> forward;
    std::multimap<std::uint64_t, std::vector<std::byte>> reverse;
    double link_free = 0.0;
    std::uint64_t next_video = 0;
    std::uint64_t next_audio = 0;
    std::uint64_t key_position = 0;
    std::optional<std::uint64_t> keyframe_due;
    std::optional<std::uint64_t> last_keyframe;
    bool keyframe_wanted = scenario.keyframe_bytes > 0;

    const auto submit = [&](MediaKind kind, std::size_t size,
                            std::uint64_t now) {
      auto bytes =
          payload(size, static_cast<std::uint8_t>(sources.size() & 0xffU));
      EXPECT_TRUE(sender.submit(kind, key_position, bytes, now));
      key_position += size;
      submit_times.push_back(now);
      sources.push_back(std::move(bytes));
      ++report.submitted;
    };

    for (std::uint64_t now = 0; now <= scenario.duration_us; now += tick) {
      while (next_video <= now) {
        const auto jitter =
            static_cast<std::size_t>(rng.next() % (scenario.frame_bytes / 2));
        auto remaining = scenario.frame_bytes * 3 / 4 + jitter;
        if (keyframe_due && now >= *keyframe_due) {
          remaining = scenario.keyframe_bytes;
          keyframe_due.reset();
          ++report.keyframes;
        }
        while (remaining > 0) {
          const auto size = std::min<std::size_t>(remaining, 1400);
          submit(MediaKind::video, size, now);
          remaining -= size;
        }
        next_video += 16667;
      }
      while (next_audio <= now) {
        submit(MediaKind::audio, 200, now);
        next_audio += 10000;
      }

      while (!forward.empty() && forward.begin()->first <= now) {
        receiver.accept(forward.begin()->second, now, report);
        forward.erase(forward.begin());
      }
      if (receiver.ack_due(now)) {
        auto record = receiver.make_ack(now);
        if (!rng.chance(scenario.ack_loss_ppm))
          reverse.emplace(now + scenario.propagation_us, std::move(record));
      }
      while (!reverse.empty() && reverse.begin()->first <= now) {
        const auto acked = sender.on_ack(reverse.begin()->second, now);
        reverse.erase(reverse.begin());
        ++report.acks;
        if (!acked) {
          report.failure = acked.error();
          report.failure_us = now;
          return report;
        }
      }

      Datagrams datagrams;
      const auto polled = sender.poll(now, datagrams);
      if (!polled) {
        report.failure = polled.error();
        report.failure_us = now;
        return report;
      }
      if (sender.take_idr_request()) {
        ++report.idr_requests;
        keyframe_wanted = scenario.keyframe_bytes > 0;
      }
      if (keyframe_wanted && !keyframe_due &&
          (!last_keyframe || now >= *last_keyframe + 1000000)) {
        keyframe_wanted = false;
        keyframe_due = now + 60000;
        last_keyframe = now;
      }
      for (auto& datagram : datagrams) {
        ++report.datagrams;
        const auto start = std::max(link_free, static_cast<double>(now));
        const auto backlog =
            (start - static_cast<double>(now)) * scenario.bandwidth;
        if (backlog > static_cast<double>(scenario.bottleneck_bytes)) continue;
        link_free =
            start + static_cast<double>(datagram.size()) / scenario.bandwidth;
        if (rng.chance(scenario.loss_ppm)) continue;
        auto arrival =
            static_cast<std::uint64_t>(link_free) + scenario.propagation_us;
        if (rng.chance(scenario.reorder_ppm))
          arrival += scenario.reorder_extra_us;
        forward.emplace(arrival, std::move(datagram));
      }

      report.max_queued_bytes =
          std::max(report.max_queued_bytes, sender.queued_bytes());
      if (const auto oldest = sender.oldest_queued_us())
        report.max_queue_age_us =
            std::max(report.max_queue_age_us, now - *oldest);
      report.max_in_flight =
          std::max(report.max_in_flight, sender.controller().bytes_in_flight());
      EXPECT_LE(sender.queued_bytes(), media_queue_bytes);
      EXPECT_LE(sender.queued_packets(), media_queue_packets);
    }
    return report;
  }
};

TEST_F(MediaSenderSimulation, DeliversAHealthyLossyPathPromptly) {
  auto scenario = lossy_path();
  scenario.bandwidth = 2.5;
  scenario.seed = 0x9e3779b97f4a7c15ULL;
  const auto report = simulate(scenario);
  EXPECT_FALSE(report.failure);
  EXPECT_GE(report.delivered * 100, report.submitted * 95);
  EXPECT_LE(report.max_queue_age_us, media_queue_age_limit_us);
  EXPECT_LE(report.max_queued_bytes, 128U * 1024U);
  EXPECT_LE(report.max_latency_us, 150000U);
}

TEST_F(MediaSenderSimulation, EndsAnOverloadedPathAfterTheGrace) {
  auto scenario = lossy_path();
  scenario.bandwidth = 0.25;
  scenario.seed = 0x2545f4914f6cdd1dULL;
  const auto report = simulate(scenario);
  EXPECT_EQ(report.failure, Error::congestion);
  EXPECT_GE(report.failure_us, media_startup_grace_us + 2000000);
  EXPECT_LT(report.failure_us, media_startup_grace_us + 4000000);
  EXPECT_GE(report.idr_requests, 1U);
  EXPECT_LE(report.max_queue_age_us, media_queue_age_limit_us);
  EXPECT_LE(report.max_queued_bytes, 256U * 1024U);
  EXPECT_LE(report.max_latency_us, 600000U);
  EXPECT_GT(report.delivered, 0U);
}

TEST_F(MediaSenderSimulation, KeepsACapablePathFastDespiteRandomLoss) {
  auto scenario = lossy_path();
  scenario.bandwidth = 2.5;
  scenario.propagation_us = 32000;
  scenario.loss_ppm = 60000;
  scenario.duration_us = 30000000;
  scenario.seed = 0x2545f4914f6cdd1dULL;
  const auto report = simulate(scenario);
  EXPECT_FALSE(report.failure);
  EXPECT_EQ(report.idr_requests, 0U);
  EXPECT_GE(report.delivered * 100, report.submitted * 85);
  EXPECT_LE(report.max_latency_us, 100000U);
}

TEST_F(MediaSenderSimulation, CarriesStartupKeyframesOverLongPaths) {
  for (const std::uint64_t round_trip :
       {20000ULL, 100000ULL, 150000ULL, 200000ULL, 250000ULL}) {
    for (const std::size_t keyframe :
         {100000UL, 200000UL, 300000UL, 500000UL}) {
      SCOPED_TRACE(round_trip);
      SCOPED_TRACE(keyframe);
      const auto report = simulate(long_path(round_trip, keyframe));
      EXPECT_FALSE(report.failure);
      EXPECT_GE(report.delivered * 100, report.submitted * 80);
      EXPECT_GE(report.keyframes, 1U);
      EXPECT_LE(report.keyframes, 6U);
      EXPECT_LE(report.max_queue_age_us, media_queue_age_limit_us);
      EXPECT_LE(report.max_latency_us,
                round_trip / 2 + media_queue_age_limit_us + 10000);
    }
  }
}

TEST_F(MediaSenderSimulation, StillEndsAStarvedLongPath) {
  auto scenario = long_path(200000, 200000);
  scenario.bandwidth = 0.05;
  const auto report = simulate(scenario);
  EXPECT_EQ(report.failure, Error::congestion);
  EXPECT_GE(report.failure_us, media_startup_grace_us + 2000000);
  EXPECT_LT(report.failure_us, media_startup_grace_us + 4000000);
}

}  // namespace
}  // namespace net_kakusan_test
