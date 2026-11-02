#include "src/core/psn/gk/measurement.hpp"

#include "src/core/utils/json/ps_reply.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

template <>
struct glz::meta<libchiaking::cloud::DatacenterPing> {
  using T = libchiaking::cloud::DatacenterPing;
  static constexpr auto value =
      glz::object("dataCenter", &T::name, "rtt", &T::rtt_ms, "rtts",
                  &T::rtts_ms, "mtu_in", &T::mtu_in, "mtu_out", &T::mtu_out,
                  "port", &T::port, "publicIp", &T::public_ip, "maxBandwidth",
                  &T::max_bandwidth, "measured", &T::measured);
};

namespace libchiaking::psn::gk {

auto Ping::from(const Datacenter& datacenter) -> Ping {
  Ping row;
  row.name = datacenter.name;
  row.public_ip = datacenter.public_ip;
  row.port = datacenter.port;
  row.max_bandwidth = datacenter.max_bandwidth;
  return row;
}

auto Ping::from(const std::vector<cloud::DatacenterPing>& values)
    -> std::vector<Ping> {
  return values | std::views::transform([](const cloud::DatacenterPing& value) {
           return Ping{value};
         }) |
         std::ranges::to<std::vector>();
}

auto Ping::published(const std::vector<Ping>& rows)
    -> std::vector<cloud::DatacenterPing> {
  return rows | std::views::transform([](const Ping& row) {
           return static_cast<const cloud::DatacenterPing&>(row);
         }) |
         std::ranges::to<std::vector>();
}

auto Ping::reported_rtt(int rtt_ms) noexcept -> int {
  const auto offset = static_cast<int>(reported_rtt_offset.count());
  const auto floor = static_cast<int>(reported_rtt_floor.count());
  return rtt_ms - offset >= floor ? rtt_ms - offset : floor;
}

auto Ping::reported() const -> Ping {
  if (!measured || rtt_ms <= 0) return *this;
  Ping row = *this;
  row.rtt_ms = reported_rtt(rtt_ms);
  std::ranges::transform(row.rtts_ms, row.rtts_ms.begin(), reported_rtt);
  return row;
}

auto Ping::reported(const std::vector<Ping>& rows) -> std::vector<Ping> {
  return rows |
         std::views::transform([](const Ping& row) { return row.reported(); }) |
         std::ranges::to<std::vector>();
}

auto Ping::serialize(const std::vector<Ping>& rows, std::size_t limit)
    -> std::optional<std::string> {
  const auto unprintable = [](const Ping& row) {
    return row.name.find('\0') != std::string::npos ||
           row.public_ip.find('\0') != std::string::npos;
  };
  if (rows.empty() || std::ranges::any_of(rows, unprintable))
    return std::nullopt;
  const auto report = published(rows);
  std::string text;
  if (glz::write<json::ps::body_options>(report, text) || text.size() > limit)
    return std::nullopt;
  return text;
}

auto Ping::parse(std::string_view report) -> std::vector<Ping> {
  const auto stored =
      json::ps::read_reply<std::vector<cloud::DatacenterPing>>(report);
  return stored ? from(*stored) : std::vector<Ping>{};
}

}  // namespace libchiaking::psn::gk

namespace libchiaking::cloud {

auto DatacenterPing::accepts_report(std::string_view json) noexcept -> bool {
  return json.size() <= prior_report_limit &&
         json.find('\0') == std::string_view::npos;
}

auto DatacenterPing::parse_report(std::string_view json)
    -> std::vector<DatacenterPing> {
  if (!accepts_report(json)) return {};
  return psn::gk::Ping::published(psn::gk::Ping::parse(json));
}

auto DatacenterPing::report(const std::vector<DatacenterPing>& pings,
                            std::size_t limit) -> std::optional<std::string> {
  return psn::gk::Ping::serialize(psn::gk::Ping::from(pings), limit);
}

}  // namespace libchiaking::cloud
