#include "src/core/net/protocol/takion/received_tos_metrics.hpp"

#include <ranges>

namespace libchiaking::net::protocol::takion {

auto ReceivedTosMetrics::pack_dscp_counts(
    const std::array<std::uint32_t, received_tos_dscp_count>& counts)
    -> std::vector<std::uint32_t> {
  return std::views::iota(std::uint32_t{0},
                          static_cast<std::uint32_t>(counts.size())) |
         std::views::filter(
             [&counts](std::uint32_t dscp) { return counts[dscp] != 0; }) |
         std::views::transform([&counts](std::uint32_t dscp) {
           return (dscp << 24U) | (counts[dscp] & 0xffffffU);
         }) |
         std::ranges::to<std::vector>();
}

void ReceivedTosMetrics::start(std::uint64_t now_us) noexcept {
  started_ = true;
  interval_start_us_ = now_us;
}

void ReceivedTosMetrics::configure(
    std::optional<std::uint32_t> packet_threshold,
    std::optional<std::uint32_t> interval_ms) noexcept {
  if (packet_threshold) packet_threshold_ = *packet_threshold;
  if (interval_ms) interval_ms_ = *interval_ms;
}

auto ReceivedTosMetrics::account(std::uint8_t first_byte,
                                 std::uint8_t traffic_class,
                                 std::uint64_t now_us)
    -> std::optional<ReceivedTosMetricsReport> {
  if (!started_ || (first_byte & 0x0fU) == received_tos_excluded_kind)
    return std::nullopt;
  const auto ecn = static_cast<std::uint8_t>(traffic_class & 0x3U);
  ++dscp_counts_[traffic_class >> 2U];
  ++ecn_counts_[ecn];
  ++total_dscp_counts_[traffic_class >> 2U];
  ++total_ecn_counts_[ecn];
  ++total_packets_;
  last_traffic_class_ = traffic_class;
  const auto elapsed_ms =
      static_cast<std::uint32_t>((now_us - interval_start_us_) / 1000U);
  bool report = elapsed_ms >= interval_ms_;
  if (!report) {
    trigger_count_ += ecn != 0 ? 1U : 0U;
    report = trigger_count_ > packet_threshold_ - 1U;
  }
  if (!report) return std::nullopt;
  ReceivedTosMetricsReport result;
  result.time_offset_msec = elapsed_ms;
  result.dscp_counts = pack_dscp_counts(dscp_counts_);
  result.ecn_ect1_count = ecn_counts_[1];
  result.ecn_ect0_count = ecn_counts_[2];
  result.ecn_ce_count = ecn_counts_[3];
  dscp_counts_.fill(0);
  ecn_counts_.fill(0);
  trigger_count_ = 0;
  interval_start_us_ = now_us;
  ++reports_;
  return result;
}

}  // namespace libchiaking::net::protocol::takion
