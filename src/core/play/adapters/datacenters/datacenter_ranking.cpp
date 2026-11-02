#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"

#include "libchiaking/detail/play/datacenter.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::play {

auto DatacenterRanking::find_ping(const std::vector<Ping>& rows,
                                  std::string_view name) -> const Ping* {
  const auto found = std::ranges::find(rows, name, &Ping::name);
  return found == rows.end() ? nullptr : &*found;
}

auto DatacenterRanking::with_selected_port(Ping row) -> Ping {
  row.selected_port = row.port > 0 ? row.port : default_port;
  return row;
}

DatacenterRanking::DatacenterRanking(std::vector<Datacenter> datacenters,
                                     std::string_view forced)
    : datacenters_(std::move(datacenters)), forced_(forced) {}

auto DatacenterRanking::rank(std::vector<Ping> pings) -> Ranking {
  std::ranges::stable_sort(pings, {}, &Ping::rtt_ms);
  if (pings.empty() || !pings.front().measured ||
      pings.front().rtt_ms > auto_max_rtt_ms)
    return {std::move(pings), std::nullopt};
  auto fastest = with_selected_port(pings.front());
  return {std::move(pings), std::move(fastest)};
}

auto DatacenterRanking::forced() const noexcept -> bool {
  return !forced_.empty() && forced_ != DatacenterName::automatic;
}

auto DatacenterRanking::forced_ping() const -> std::optional<Ping> {
  if (!forced()) return std::nullopt;
  return placeholder_ping(forced_);
}

auto DatacenterRanking::picker_rows(const std::vector<Ping>& current,
                                    const std::vector<Ping>& prior) const
    -> std::vector<Ping> {
  std::vector<Ping> out;
  out.reserve(datacenters_.size() + prior.size());

  for (const auto& datacenter : datacenters_) {
    const bool forced_dummy = forced() && datacenter.name == forced_;
    if (!forced_dummy) {
      if (const auto* row = find_ping(current, datacenter.name)) {
        out.push_back(*row);
        continue;
      }
    }
    if (const auto* row = find_ping(prior, datacenter.name)) {
      out.push_back(*row);
      continue;
    }
    if (forced_dummy) {
      if (const auto* row = find_ping(current, datacenter.name)) {
        out.push_back(*row);
        continue;
      }
    }
    auto placeholder = Ping::from(datacenter);
    placeholder.rtts_ms = {0};
    out.push_back(std::move(placeholder));
  }
  for (const auto& row : prior) {
    if (!row.name.empty() && !find_ping(out, row.name)) out.push_back(row);
  }
  return out;
}

auto DatacenterRanking::choose(std::vector<Ping> completed,
                               const std::vector<Ping>& prior) const
    -> std::optional<Candidates> {
  if (forced()) {
    auto ping = forced_ping();
    if (!ping) return std::nullopt;
    std::vector<Ping> results{*ping};
    auto picker = picker_rows(results, prior);
    return Candidates{std::move(ping), std::move(results), std::move(picker)};
  }

  auto ranking = rank(std::move(completed));
  auto picker = picker_rows(ranking.sorted, prior);
  return Candidates{std::move(ranking.fastest), std::move(ranking.sorted),
                    std::move(picker)};
}

auto DatacenterRanking::offer() const -> std::vector<std::string> {
  std::vector<std::string> names;
  names.reserve(datacenters_.size());
  for (const auto& datacenter : datacenters_)
    if (!datacenter.name.empty() &&
        std::ranges::find(names, datacenter.name) == names.end())
      names.push_back(datacenter.name);
  return names;
}

auto DatacenterRanking::pick(std::string_view name, Candidates choice) const
    -> std::optional<Candidates> {
  std::optional<Ping> selected;
  const auto measured = std::ranges::find_if(
      choice.ping_results,
      [name](const Ping& row) { return row.name == name && row.measured; });
  if (measured != choice.ping_results.end())
    selected = with_selected_port(*measured);
  else if (!name.empty() && name != DatacenterName::automatic)
    selected = placeholder_ping(name);
  if (!selected) return std::nullopt;
  choice.ping_results = {*selected};
  choice.selected = std::move(selected);
  return choice;
}

auto DatacenterRanking::placeholder_ping(std::string_view name) const
    -> std::optional<Ping> {
  const auto found = std::ranges::find(datacenters_, name, &Datacenter::name);
  if (found == datacenters_.end()) return std::nullopt;
  auto ping = Ping::from(*found);
  ping.rtt_ms = forced_rtt_ms;
  ping.rtts_ms = {forced_rtt_ms};
  ping.mtu_in = Ping::default_mtu_in;
  ping.mtu_out = Ping::default_mtu_out;
  ping.measured = true;
  return with_selected_port(std::move(ping));
}

}  // namespace libchiaking::play
