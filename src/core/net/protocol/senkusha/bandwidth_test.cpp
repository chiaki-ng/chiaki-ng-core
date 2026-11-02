#include "src/core/net/protocol/senkusha/bandwidth_test.hpp"

#include <libchiaking/platform/runtime/time_units.hpp>

#include <algorithm>
#include <cmath>

namespace libchiaking::net::protocol::senkusha {

namespace {
constexpr double bits_per_byte = 8.0;
constexpr double microseconds_per_kilobit_second = 1000.0;
}  // namespace

BandwidthTest::BandwidthTest(const BandwidthPolicy& policy) noexcept
    : policy_(policy) {}

auto BandwidthTest::create(const BandwidthPolicy& policy)
    -> Expected<BandwidthTest, Error> {
  const bool rates = policy.preliminary_kbps > 0 &&
                     policy.target_kbps >= policy.preliminary_kbps;
  const bool durations =
      policy.preliminary_duration.count() > 0 && policy.duration.count() > 0 &&
      policy.retry_delay.count() >= 0 && policy.reply_grace.count() > 0;
  const bool loss = std::isfinite(policy.maximum_loss) &&
                    policy.maximum_loss >= 0.0 && policy.maximum_loss <= 1.0;
  if (!rates || !durations || !loss || policy.fps == 0 ||
      policy.minimum_step_kbps == 0 || policy.maximum_rounds < 2)
    return unexpected(Error::invalid_configuration);
  return BandwidthTest{policy};
}

auto BandwidthTest::start(std::uint64_t now_us) -> BandwidthStep {
  if (phase_ != Phase::idle) return bandwidth_step::Finished{std::nullopt};
  return begin(Phase::preliminary, policy_.preliminary_kbps,
               static_cast<std::uint32_t>(policy_.preliminary_duration.count()),
               now_us);
}

void BandwidthTest::packet(std::uint64_t now_us, std::uint64_t bytes) noexcept {
  if (phase_ != Phase::preliminary && phase_ != Phase::main) return;
  ++received_packets_;
  received_bytes_ += bytes;
  if (!first_packet_us_) first_packet_us_ = now_us;
  last_packet_us_ = now_us;
}

auto BandwidthTest::reply(std::uint32_t round_id, std::uint32_t total_sent,
                          std::uint64_t now_us)
    -> std::optional<BandwidthStep> {
  if (!round_ || round_->id != round_id ||
      (phase_ != Phase::preliminary && phase_ != Phase::main))
    return std::nullopt;
  if (phase_ == Phase::preliminary) {
    if (received_packets_ == 0) return conclude(std::nullopt);
    return begin(Phase::main, policy_.target_kbps,
                 static_cast<std::uint32_t>(policy_.duration.count()), now_us);
  }
  if (total_sent == 0) return conclude(delivered(measure(0)));
  return settle(measure(total_sent), now_us);
}

auto BandwidthTest::tick(std::uint64_t now_us) -> std::optional<BandwidthStep> {
  if (phase_ == Phase::resting && now_us >= rest_until_us_)
    return begin(Phase::main, next_kbps_,
                 static_cast<std::uint32_t>(policy_.duration.count()), now_us);
  if ((phase_ != Phase::preliminary && phase_ != Phase::main) ||
      now_us < round_deadline_us_)
    return std::nullopt;
  if (received_packets_ == 0) return conclude(std::nullopt);
  if (phase_ == Phase::preliminary)
    return begin(Phase::main, policy_.target_kbps,
                 static_cast<std::uint32_t>(policy_.duration.count()), now_us);
  return conclude(delivered(measure(0)));
}

auto BandwidthTest::deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  switch (phase_) {
    case Phase::preliminary:
    case Phase::main:
      return round_deadline_us_;
    case Phase::resting:
      return rest_until_us_;
    case Phase::idle:
    case Phase::finished:
      return std::nullopt;
  }
  return std::nullopt;
}

auto BandwidthTest::begin(Phase phase, std::uint32_t kbps,
                          std::uint32_t duration_ms, std::uint64_t now_us)
    -> BandwidthStep {
  if (rounds_ >= policy_.maximum_rounds) return conclude(std::nullopt);
  ++rounds_;
  round_ = BandwidthRound{next_id_++, kbps, duration_ms, policy_.fps};
  phase_ = phase;
  received_packets_ = 0;
  received_bytes_ = 0;
  first_packet_us_.reset();
  last_packet_us_ = 0;
  round_deadline_us_ =
      now_us + (static_cast<std::uint64_t>(duration_ms) +
                static_cast<std::uint64_t>(policy_.reply_grace.count())) *
                   microseconds_per_millisecond;
  return bandwidth_step::Send{*round_};
}

auto BandwidthTest::measure(std::uint32_t total_sent) const noexcept
    -> BandwidthMeasurement {
  const auto requested = round_ ? round_->kbps : 0U;
  const auto window_us =
      first_packet_us_ && last_packet_us_ > *first_packet_us_
          ? last_packet_us_ - *first_packet_us_
          : static_cast<std::uint64_t>(round_ ? round_->duration_ms : 1U) *
                microseconds_per_millisecond;
  const double kbps =
      static_cast<double>(received_bytes_) * bits_per_byte *
      microseconds_per_kilobit_second /
      static_cast<double>(std::max<std::uint64_t>(window_us, 1));
  const double capped = std::min(kbps, static_cast<double>(requested));
  double loss = 0.0;
  if (total_sent > 0 && received_packets_ < total_sent)
    loss = 1.0 - static_cast<double>(received_packets_) /
                     static_cast<double>(total_sent);
  return BandwidthMeasurement{static_cast<std::uint32_t>(capped), loss,
                              requested};
}

auto BandwidthTest::conclude(std::optional<BandwidthMeasurement> measurement)
    -> BandwidthStep {
  phase_ = Phase::finished;
  round_.reset();
  return bandwidth_step::Finished{measurement};
}

auto BandwidthTest::delivered(const BandwidthMeasurement& measurement) noexcept
    -> std::optional<BandwidthMeasurement> {
  if (measurement.kbps == 0) return std::nullopt;
  return measurement;
}

auto BandwidthTest::settle(const BandwidthMeasurement& measurement,
                           std::uint64_t now_us) -> BandwidthStep {
  if (measurement.loss <= policy_.maximum_loss)
    return conclude(BandwidthMeasurement{measurement.requested_kbps,
                                         measurement.loss,
                                         measurement.requested_kbps});
  const auto stepped =
      measurement.requested_kbps > policy_.minimum_step_kbps
          ? measurement.requested_kbps - policy_.minimum_step_kbps
          : 0U;
  const auto next = std::min(measurement.kbps, stepped);
  if (next < policy_.preliminary_kbps || rounds_ >= policy_.maximum_rounds)
    return conclude(delivered(measurement));
  phase_ = Phase::resting;
  next_kbps_ = next;
  rest_until_us_ =
      now_us + static_cast<std::uint64_t>(policy_.retry_delay.count()) *
                   microseconds_per_millisecond;
  round_.reset();
  return bandwidth_step::Wait{rest_until_us_};
}

}  // namespace libchiaking::net::protocol::senkusha
