#include "src/core/net/kakusan/timing.hpp"

#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>

namespace libchiaking::net::kakusan {

auto Freshness::accept(std::uint32_t sequence) noexcept -> bool {
  if (seen_ && !ControllerFrameCodec::is_newer(sequence, last_)) return false;
  seen_ = true;
  last_ = sequence;
  return true;
}

void StatePacer::start(std::uint64_t now_us) noexcept {
  started_ = true;
  sent_ = false;
  start_ = now_us;
}

void StatePacer::stop() noexcept { started_ = false; }

void StatePacer::mark(std::uint64_t now_us) noexcept {
  sent_ = true;
  last_ = now_us;
}

auto StatePacer::next_send_us(bool changed, bool periodic) const noexcept
    -> std::optional<std::uint64_t> {
  if (!started_ || (!changed && !periodic)) return std::nullopt;
  if (!sent_) return start_;
  return timing::Deadline::after_us(
      last_, changed ? state_min_gap_us : state_period_us);
}

void Heartbeat::start(std::uint64_t now_us) noexcept {
  running_ = true;
  outstanding_ = false;
  last_incoming_ = now_us;
  next_ping_ = timing::Deadline::after_us(now_us, ping_interval_us);
}

void Heartbeat::stop() noexcept {
  running_ = false;
  outstanding_ = false;
  nonce_ = {};
}

void Heartbeat::refresh(std::uint64_t now_us) noexcept {
  if (running_) last_incoming_ = std::max(last_incoming_, now_us);
}

auto Heartbeat::silent(std::uint64_t now_us) const noexcept -> bool {
  return running_ && now_us >= timing::Deadline::after_us(last_incoming_,
                                                          liveness_timeout_us);
}

auto Heartbeat::ping_due(std::uint64_t now_us) const noexcept -> bool {
  return running_ && now_us >= next_ping_;
}

void Heartbeat::issue(const control::Nonce& nonce,
                      std::uint64_t now_us) noexcept {
  outstanding_ = true;
  std::ranges::copy(nonce, nonce_.begin());
  next_ping_ = timing::Deadline::after_us(next_ping_, ping_interval_us);
  if (next_ping_ <= now_us)
    next_ping_ = timing::Deadline::after_us(now_us, ping_interval_us);
}

auto Heartbeat::answer(const control::Nonce& nonce) noexcept -> bool {
  if (!running_ || !outstanding_ ||
      !memory::constant_time_equal(nonce, std::span<const std::byte>{nonce_}))
    return false;
  outstanding_ = false;
  return true;
}

auto Heartbeat::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (!running_) return std::nullopt;
  return std::min(
      timing::Deadline::after_us(last_incoming_, liveness_timeout_us),
      next_ping_);
}

}  // namespace libchiaking::net::kakusan
