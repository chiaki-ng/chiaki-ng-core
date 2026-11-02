#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace libchiaking::play {

class LaunchObserver::Silent final : public LaunchObserver {
 public:
  void on_progress(const LaunchProgress&,
                   net::protocol::remote_play::FailureStage,
                   std::string_view) override {}
};

auto LaunchObserver::silent() noexcept -> LaunchObserver& {
  static Silent observer;
  return observer;
}

void LaunchObserver::on_datacenters(const std::vector<Ping>&,
                                    const std::vector<Ping>&) {}

void LaunchObserver::on_datacenter_selected(const Ping&, bool) {}

auto LaunchObserver::selects_datacenter_at_launch() const noexcept -> bool {
  return false;
}

void LaunchObserver::offer_datacenters(const std::vector<std::string>&) {}

auto LaunchObserver::await_datacenter_choice(const platform::StopToken& stop)
    -> LaunchChoice {
  return {stop.stop_requested() ? LaunchChoice::Kind::cancelled
                                : LaunchChoice::Kind::timed_out,
          {}};
}

auto LaunchObserver::datacenter_choice_wait() const noexcept
    -> std::optional<std::chrono::milliseconds> {
  return std::nullopt;
}

auto LaunchObserver::cached_bandwidth(const Ping&)
    -> std::optional<CachedBandwidth> {
  return std::nullopt;
}

void LaunchObserver::on_bandwidth_measured(const Ping&, int) {}

LaunchReport::LaunchReport() noexcept : observer_(&LaunchObserver::silent()) {}

void LaunchReport::say(LaunchActivity activity,
                       net::protocol::remote_play::FailureStage stage,
                       std::string_view text, Counts counts) const {
  if (const auto progress = LaunchProgress::at(service_, activity, counts))
    observer_->on_progress(*progress, stage, text);
}

void LaunchReport::say(LaunchActivity activity,
                       net::protocol::remote_play::FailureStage stage,
                       std::string text, std::size_t capacity,
                       Counts counts) const {
  say(activity, stage, text::truncate(std::move(text), capacity), counts);
}

void LaunchReport::step(LaunchActivity activity,
                        net::protocol::remote_play::FailureStage stage,
                        std::string_view label, int step, int total) const {
  say(activity, stage, step_text(label, step, total), counted(step, total));
}

void LaunchReport::step(LaunchActivity activity,
                        net::protocol::remote_play::FailureStage stage,
                        std::string_view label, int step, int total,
                        std::size_t capacity) const {
  say(activity, stage, step_text(label, step, total), capacity,
      counted(step, total));
}

auto LaunchReport::step_text(std::string_view label, int step, int total)
    -> std::string {
  return std::string{label} + " - Step " + std::to_string(step) + " of " +
         std::to_string(total);
}

auto LaunchReport::counted(int step, int total) noexcept -> Counts {
  Counts counts;
  if (step >= 1 && step <= total)
    counts.step = LaunchProgress::Step{static_cast<std::uint32_t>(step),
                                       static_cast<std::uint32_t>(total)};
  return counts;
}

auto LaunchReport::attempted(int attempt) noexcept -> Counts {
  Counts counts;
  if (attempt >= 1) counts.attempt = static_cast<std::uint32_t>(attempt);
  return counts;
}

auto LaunchReport::waiting(progress::WaitReason reason, int attempt,
                           int delay_seconds) noexcept -> Counts {
  auto counts = attempted(attempt);
  counts.wait =
      progress::Wait{reason, std::chrono::seconds{std::max(delay_seconds, 0)}};
  return counts;
}

auto LaunchReport::measured(std::size_t done, std::size_t total, int step,
                            int steps) noexcept -> Counts {
  auto counts = counted(step, steps);
  counts.measured = LaunchProgress::Step{static_cast<std::uint32_t>(done),
                                         static_cast<std::uint32_t>(total)};
  return counts;
}

}  // namespace libchiaking::play
