#include "src/core/utils/timing/token_bucket.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>

namespace libchiaking::timing {

TokenBucket::TokenBucket(std::uint64_t tokens_per_second,
                         std::uint64_t capacity, std::uint64_t now_us) noexcept
    : rate_(tokens_per_second),
      capacity_(capacity * scale),
      tokens_(capacity * scale),
      updated_us_(now_us) {}

void TokenBucket::refill(std::uint64_t now_us) noexcept {
  if (now_us <= updated_us_) return;
  const auto elapsed = std::min(now_us - updated_us_, capacity_ / rate_ + 1);
  tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
  updated_us_ = now_us;
}

auto TokenBucket::holds(std::uint64_t amount) const noexcept -> bool {
  return tokens_ >= amount * scale;
}

auto TokenBucket::take(std::uint64_t amount) noexcept -> bool {
  if (!holds(amount)) return false;
  tokens_ -= amount * scale;
  return true;
}

auto TokenBucket::ready_at_us(std::uint64_t amount) const noexcept
    -> std::uint64_t {
  if (holds(amount)) return updated_us_;
  const auto deficit = amount * scale - tokens_;
  return timing::Deadline::after_us(updated_us_, (deficit + rate_ - 1) / rate_);
}

}  // namespace libchiaking::timing
