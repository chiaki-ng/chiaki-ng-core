#include "src/core/net/kakusan/congestion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace libchiaking::net::kakusan {
auto CongestionArithmetic::as_double(std::uint64_t value) noexcept -> double {
  return static_cast<double>(value);
}

auto CongestionArithmetic::ceil_time(double value) noexcept -> std::uint64_t {
  if (!(value > 0.0)) return 0;
  if (value >= max_time) return std::numeric_limits<std::uint64_t>::max();
  return std::chrono::ceil<std::chrono::duration<std::uint64_t, std::micro>>(
             std::chrono::duration<double, std::micro>{value})
      .count();
}

auto CongestionController::exhausted() const noexcept -> bool {
  return next_ == std::numeric_limits<std::uint64_t>::max();
}

auto CongestionController::can_track() const noexcept -> bool {
  return !exhausted() && sent_.size() < max_tracked_packets;
}

auto CongestionController::path() const noexcept -> PathState {
  if (delay_ == PathState::congested || loss_ == PathState::congested)
    return PathState::congested;
  return PathState::clear;
}

auto CongestionController::delay_floor_us() const noexcept -> double {
  if (!floor_) return 0.0;
  if (!earlier_floor_) return floor_->rtt_us;
  return std::min(floor_->rtt_us, earlier_floor_->rtt_us);
}

auto CongestionController::delay_limit_us() const noexcept -> double {
  const auto floor = delay_floor_us();
  return floor + std::max(delay_margin_floor_us, floor);
}

auto CongestionController::delay_release_us() const noexcept -> double {
  const auto floor = delay_floor_us();
  return floor +
         std::max(delay_margin_floor_us, floor) * delay_release_fraction;
}

auto CongestionController::on_packet_sent(std::size_t bytes, bool probe,
                                          std::uint64_t now_us)
    -> Result<std::uint64_t> {
  if (exhausted() || bytes == 0 || bytes > congestion_datagram_size)
    return fail(Error::protocol);
  const auto number = next_++;
  sent_.push_back(SentPacket{now_us, static_cast<std::uint32_t>(bytes),
                             PacketState::outstanding});
  in_flight_ += bytes;
  last_ack_eliciting_ = now_us;
  if (probe) probe_pending_ = false;
  return number;
}

auto CongestionController::entry(std::uint64_t number) noexcept -> SentPacket& {
  return sent_[static_cast<std::size_t>(number - base_)];
}

auto CongestionController::entry(std::uint64_t number) const noexcept
    -> const SentPacket& {
  return sent_[static_cast<std::size_t>(number - base_)];
}

auto CongestionController::on_ack(const MediaAck& ack, std::uint64_t now_us)
    -> Result<AckOutcome> {
  if (!ack.well_formed() || ack.largest >= next_) return fail(Error::protocol);

  largest_acked_ =
      largest_acked_ ? std::max(*largest_acked_, ack.largest) : ack.largest;

  std::vector<Settled> acked;
  std::optional<Settled> largest_new;
  for (const auto& range : ack.ranges) {
    if (range.high < base_) continue;
    for (auto number = std::max(range.low, base_); number <= range.high;
         ++number) {
      auto& packet = entry(number);
      if (packet.state != PacketState::outstanding) continue;
      packet.state = PacketState::acked;
      const Settled settled{number, packet.time_sent, packet.bytes};
      acked.push_back(settled);
      if (!largest_new || number > largest_new->number) largest_new = settled;
    }
  }

  AckOutcome outcome;
  if (acked.empty()) return outcome;
  outcome.newly_acked = acked.size();

  if (largest_new->number == ack.largest) {
    latest_rtt_ =
        now_us > largest_new->time_sent
            ? CongestionArithmetic::as_double(now_us - largest_new->time_sent)
            : 0.0;
    update_rtt(std::min(CongestionArithmetic::as_double(ack.delay_us),
                        peer_ack_delay_limit_us),
               now_us);
    outcome.rtt_sampled = true;
  }

  const auto lost = detect_lost(now_us);
  outcome.lost = lost.size();
  on_packets_lost(lost);
  for (const auto& packet : acked) in_flight_ -= packet.bytes;
  note_settled(acked.size(), lost.size(), now_us);

  pto_count_ = 0;
  trim();
  return outcome;
}

void CongestionController::update_rtt(double ack_delay_us,
                                      std::uint64_t now_us) noexcept {
  if (!first_sample_) {
    min_rtt_ = latest_rtt_;
    smoothed_rtt_ = latest_rtt_;
    rtt_variance_ = latest_rtt_ / initial_rttvar_divisor;
    first_sample_ = now_us;
    note_delay(now_us);
    return;
  }
  min_rtt_ = std::min(min_rtt_, latest_rtt_);
  auto adjusted = latest_rtt_;
  if (latest_rtt_ >= min_rtt_ + ack_delay_us)
    adjusted = latest_rtt_ - ack_delay_us;
  rtt_variance_ = rttvar_retain_weight * rtt_variance_ +
                  rttvar_sample_weight * std::abs(smoothed_rtt_ - adjusted);
  smoothed_rtt_ =
      srtt_retain_weight * smoothed_rtt_ + srtt_sample_weight * adjusted;
  note_delay(now_us);
}

void CongestionController::note_delay(std::uint64_t now_us) noexcept {
  const auto window = now_us / delay_floor_window_us;
  if (floor_ && floor_->window == window) {
    floor_->rtt_us = std::min(floor_->rtt_us, latest_rtt_);
  } else {
    if (floor_ && floor_->window + 1 == window)
      earlier_floor_ = floor_;
    else
      earlier_floor_.reset();
    floor_ = DelayFloor{window, latest_rtt_};
  }
  switch (delay_) {
    case PathState::clear:
      if (smoothed_rtt_ > delay_limit_us()) delay_ = PathState::congested;
      break;
    case PathState::congested:
      if (smoothed_rtt_ < delay_release_us()) delay_ = PathState::clear;
      break;
  }
}

void CongestionController::note_settled(std::size_t delivered, std::size_t lost,
                                        std::uint64_t now_us) noexcept {
  const auto window = now_us / loss_window_us;
  if (settled_.window != window) {
    judge_loss();
    settled_ = LossWindow{window, 0, 0};
  }
  settled_.delivered += delivered;
  settled_.lost += lost;
}

void CongestionController::judge_loss() noexcept {
  const auto total = settled_.delivered + settled_.lost;
  if (total < loss_window_packets) {
    loss_ = PathState::clear;
    return;
  }
  switch (loss_) {
    case PathState::clear:
      if (settled_.lost * 100 > total * loss_limit_percent)
        loss_ = PathState::congested;
      break;
    case PathState::congested:
      if (settled_.lost * 100 < total * loss_release_percent)
        loss_ = PathState::clear;
      break;
  }
}

auto CongestionController::detect_lost(std::uint64_t now_us)
    -> std::vector<Settled> {
  loss_time_.reset();
  std::vector<Settled> lost;
  if (!largest_acked_ || next_ == 0) return lost;
  const auto loss_delay =
      std::max(time_threshold * std::max(latest_rtt_, smoothed_rtt_),
               timer_granularity_us);
  const auto lost_send_time =
      CongestionArithmetic::as_double(now_us) - loss_delay;
  const auto limit = std::min(*largest_acked_, next_ - 1);
  for (auto number = base_; number <= limit && number < next_; ++number) {
    auto& packet = entry(number);
    if (packet.state != PacketState::outstanding) continue;
    if (CongestionArithmetic::as_double(packet.time_sent) <=
            lost_send_time + CongestionArithmetic::tolerance ||
        *largest_acked_ - number >= packet_threshold) {
      packet.state = PacketState::lost;
      lost.push_back(Settled{number, packet.time_sent, packet.bytes});
      continue;
    }
    const auto candidate = CongestionArithmetic::ceil_time(
        CongestionArithmetic::as_double(packet.time_sent) + loss_delay);
    loss_time_ = loss_time_ ? std::min(*loss_time_, candidate) : candidate;
  }
  return lost;
}

void CongestionController::on_packets_lost(
    const std::vector<Settled>& lost) noexcept {
  for (const auto& packet : lost) in_flight_ -= packet.bytes;
  lost_packets_ += lost.size();
}

auto CongestionController::pto_deadline() const noexcept
    -> std::optional<std::uint64_t> {
  if (in_flight_ == 0 || !last_ack_eliciting_) return std::nullopt;
  const auto backoff = std::ldexp(
      1.0, static_cast<int>(std::min(pto_count_, max_pto_backoff_exponent)));
  const auto duration =
      (smoothed_rtt_ +
       std::max(pto_variance_multiplier * rtt_variance_, timer_granularity_us) +
       peer_ack_delay_limit_us) *
      backoff;
  return CongestionArithmetic::ceil_time(
      CongestionArithmetic::as_double(*last_ack_eliciting_) + duration);
}

auto CongestionController::loss_detection_deadline() const noexcept
    -> std::optional<std::uint64_t> {
  if (loss_time_) return loss_time_;
  return pto_deadline();
}

auto CongestionController::on_timeout(std::uint64_t now_us) -> TimeoutEvent {
  const auto deadline = loss_detection_deadline();
  if (!deadline || *deadline > now_us) return TimeoutEvent::none;
  if (loss_time_) {
    const auto lost = detect_lost(now_us);
    on_packets_lost(lost);
    note_settled(0, lost.size(), now_us);
    trim();
    return TimeoutEvent::loss;
  }
  probe_pending_ = true;
  ++pto_count_;
  return TimeoutEvent::probe;
}

void CongestionController::trim() noexcept {
  while (!sent_.empty() && sent_.front().state != PacketState::outstanding) {
    sent_.pop_front();
    ++base_;
  }
}

void CongestionController::reset() noexcept {
  sent_.clear();
  base_ = 0;
  next_ = 0;
  in_flight_ = 0;
  lost_packets_ = 0;
  latest_rtt_ = 0.0;
  smoothed_rtt_ = initial_rtt_us;
  rtt_variance_ = initial_rtt_us / initial_rttvar_divisor;
  min_rtt_ = 0.0;
  first_sample_.reset();
  largest_acked_.reset();
  last_ack_eliciting_.reset();
  loss_time_.reset();
  floor_.reset();
  earlier_floor_.reset();
  settled_ = LossWindow{};
  delay_ = PathState::clear;
  loss_ = PathState::clear;
  pto_count_ = 0;
  probe_pending_ = false;
}

auto Pacer::ceiling() const noexcept -> double {
  if (idle_) return pacer_initial_tokens;
  return std::max(pacer_capacity, rate_ * pacer_catch_up_us);
}

auto Pacer::refilled(std::uint64_t now_us) const noexcept -> double {
  const auto limit = ceiling();
  if (now_us <= updated_ || tokens_ >= limit) return tokens_;
  return std::min(limit, tokens_ + rate_ * CongestionArithmetic::as_double(
                                               now_us - updated_));
}

void Pacer::refill(std::uint64_t now_us) noexcept {
  tokens_ = refilled(now_us);
  updated_ = std::max(updated_, now_us);
}

void Pacer::set_rate(double bytes_per_us, std::uint64_t now_us) noexcept {
  refill(now_us);
  rate_ = bytes_per_us;
}

void Pacer::set_idle(bool idle, std::uint64_t now_us) noexcept {
  refill(now_us);
  idle_ = idle;
}

auto Pacer::tokens(std::uint64_t now_us) const noexcept -> double {
  return refilled(now_us);
}

auto Pacer::can_send(std::size_t bytes, std::uint64_t now_us) const noexcept
    -> bool {
  return tokens(now_us) + CongestionArithmetic::tolerance >=
         CongestionArithmetic::as_double(bytes);
}

void Pacer::on_sent(std::size_t bytes, std::uint64_t now_us) noexcept {
  refill(now_us);
  tokens_ -= CongestionArithmetic::as_double(bytes);
}

auto Pacer::next_send_time(std::size_t bytes) const noexcept -> std::uint64_t {
  const auto deficit = CongestionArithmetic::as_double(bytes) - tokens_;
  if (deficit <= CongestionArithmetic::tolerance) return updated_;
  if (!(rate_ > 0.0)) return std::numeric_limits<std::uint64_t>::max();
  return CongestionArithmetic::ceil_time(
      CongestionArithmetic::as_double(updated_) + std::ceil(deficit / rate_));
}

void Pacer::reset(std::uint64_t now_us) noexcept {
  tokens_ = pacer_initial_tokens;
  rate_ = pacer_initial_rate;
  updated_ = now_us;
  idle_ = true;
}

}  // namespace libchiaking::net::kakusan
