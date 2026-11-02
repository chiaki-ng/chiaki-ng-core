#include "src/core/net/protocol/senkusha/mtu_search.hpp"

#include <limits>

namespace libchiaking::net::protocol::senkusha {

auto ConservativeMtuSearch::create(std::uint32_t lower, std::uint32_t upper,
                                   std::uint32_t attempts_per_candidate,
                                   std::uint32_t initial_candidate,
                                   std::uint64_t initial_attempt_id)
    -> Expected<ConservativeMtuSearch, Error> {
  if (lower >= upper || attempts_per_candidate == 0 ||
      initial_candidate < lower || initial_candidate > upper ||
      initial_attempt_id == 0) {
    return unexpected(Error::invalid_configuration);
  }
  ConservativeMtuSearch search;
  search.lower_ = lower;
  search.upper_ = upper;
  search.candidate_ = initial_candidate;
  search.attempts_per_candidate_ = attempts_per_candidate;
  search.next_id_ = initial_attempt_id;
  return search;
}

auto ConservativeMtuSearch::next_attempt()
    -> Expected<MtuSearchAttempt, Error> {
  if (complete() || outstanding_id_) return unexpected(Error::invalid_state);
  if (next_id_ == 0 ||
      attempts_used_ == std::numeric_limits<std::uint32_t>::max() ||
      attempts_on_candidate_ == std::numeric_limits<std::uint32_t>::max()) {
    return unexpected(Error::arithmetic_overflow);
  }
  const auto id = next_id_++;
  outstanding_id_ = id;
  ++attempts_used_;
  ++attempts_on_candidate_;
  return MtuSearchAttempt{id, candidate_, attempts_on_candidate_};
}

auto ConservativeMtuSearch::record_success(std::uint64_t id)
    -> Expected<void, Error> {
  if (!matches_outstanding(id)) return unexpected(Error::invalid_state);
  outstanding_id_.reset();
  lower_ = candidate_;
  attempts_on_candidate_ = 0;
  select_midpoint();
  return {};
}

auto ConservativeMtuSearch::record_failure(std::uint64_t id)
    -> Expected<void, Error> {
  if (!matches_outstanding(id)) return unexpected(Error::invalid_state);
  outstanding_id_.reset();
  if (attempts_on_candidate_ < attempts_per_candidate_) return {};
  attempts_on_candidate_ = 0;
  upper_ = candidate_;
  select_midpoint();
  return {};
}

MtuSearchSnapshot ConservativeMtuSearch::snapshot() const noexcept {
  return MtuSearchSnapshot{
      lower_, upper_, candidate_, attempts_used_,
      complete() ? std::optional<std::uint32_t>{lower_} : std::nullopt};
}

bool ConservativeMtuSearch::complete() const noexcept {
  return upper_ - lower_ <= 1;
}

void ConservativeMtuSearch::select_midpoint() noexcept {
  if (!complete()) candidate_ = lower_ + (upper_ - lower_) / 2;
}

bool ConservativeMtuSearch::matches_outstanding(
    std::uint64_t id) const noexcept {
  return outstanding_id_ && *outstanding_id_ == id;
}

}  // namespace libchiaking::net::protocol::senkusha
