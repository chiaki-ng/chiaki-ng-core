#pragma once

#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/play/adapters/links/cloud/server_slot.hpp"

#include <libchiaking/detail/play/datacenter.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include "src/core/psn/server_clock.hpp"
#include "tests/support/play/scripted_clearance.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/utc_offset.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::play {

class RecordingListener final
    : public libchiaking::play::CloudLaunch::Core::Listener {
 public:
  using Ping = libchiaking::psn::gk::Ping;

  struct Chosen {
    std::optional<libchiaking::play::DatacenterChoice> choice;
    std::string name;
    bool forced{};
    int reported_rtt_ms{};
  };

  RecordingListener() = default;
  explicit RecordingListener(libchiaking::play::LaunchObserver& forward)
      : forward_(&forward) {}

  void progressed(const libchiaking::play::LaunchProgress& progress,
                  libchiaking::net::protocol::remote_play::FailureStage stage,
                  std::string_view text) override {
    typed.push_back(progress);
    texts.emplace_back(text);
    if (forward_ != nullptr) forward_->on_progress(progress, stage, text);
  }

  void datacenters_measured(const std::vector<Ping>& picker_rows) override {
    measured.push_back(picker_rows);
  }

  void datacenters_offered(
      const libchiaking::play::DatacenterOffer& offer) override {
    offers.push_back(offer);
  }

  void datacenter_chosen(
      const std::optional<libchiaking::play::DatacenterChoice>& choice,
      const Ping& selected, bool forced, int reported_rtt_ms) override {
    chosen.push_back({choice, selected.name, forced, reported_rtt_ms});
  }

  void idle(std::chrono::microseconds) override {
    ++idles;
    if (on_idle) on_idle();
  }

  void wake() noexcept override { ++wakes; }

  void note(std::string_view message) noexcept override {
    notes.emplace_back(message);
  }

  std::vector<libchiaking::play::LaunchProgress> typed;
  std::vector<std::string> texts;
  std::vector<std::vector<Ping>> measured;
  std::vector<libchiaking::play::DatacenterOffer> offers;
  std::vector<Chosen> chosen;
  std::vector<std::string> notes;
  std::function<void()> on_idle;
  int idles{};
  int wakes{};

 private:
  libchiaking::play::LaunchObserver* forward_{};
};

class MemoryDatacenterStore final : public libchiaking::play::DatacenterStore {
 public:
  using Service = libchiaking::catalog::ServiceGeneration;

  auto load(Service service) -> libchiaking::Expected<
      std::vector<libchiaking::play::DatacenterMeasurement>, Error> override {
    ++loads;
    if (failing) return libchiaking::unexpected(Error::unavailable);
    return held[service];
  }

  auto save(
      Service service,
      std::span<const libchiaking::play::DatacenterMeasurement> measurements)
      -> libchiaking::Expected<void, Error> override {
    ++saves;
    if (failing) return libchiaking::unexpected(Error::unavailable);
    held[service].assign(measurements.begin(), measurements.end());
    return {};
  }

  std::map<Service, std::vector<libchiaking::play::DatacenterMeasurement>> held;
  int loads{};
  int saves{};
  bool failing{};
};

struct LaunchCase {
  std::shared_ptr<libchiaking::users::PsUser> user;
  Title title{.product = "synthetic-entitlement"};
  std::optional<libchiaking::catalog::LaunchRefusal> refusal{};
  libchiaking::play::DatacenterPreference datacenter{named("dc1")};
  std::string time_zone{"UTC+00:00"};
  int resolution{1080};
  int bitrate_kbps{15000};
  std::uint32_t takion_version_override{};
  bool skip_account_attributes_check{};
  libchiaking::platform::StopToken stop{};
  std::optional<libchiaking::catalog::ClearedLaunch> cleared{};
  FixedUtcOffset utc_offset{std::chrono::minutes{0}};

  static auto named(std::string_view name)
      -> libchiaking::play::DatacenterPreference {
    return libchiaking::play::DatacenterPreference::named(
        Setup::value(libchiaking::play::DatacenterName::parse(name),
                     "the datacenter name parses"));
  }

  auto clearance() const -> ScriptedClearance {
    return refusal ? ScriptedClearance{*refusal} : ScriptedClearance{title};
  }

  auto request() const -> libchiaking::play::CloudLaunch::Core::Request {
    libchiaking::play::CloudLaunch::Core::Request request{
        user, Title::product_id(title.product)};
    request.datacenter = datacenter;
    request.time_zone = time_zone;
    request.resolution = resolution;
    request.bitrate_kbps = bitrate_kbps;
    request.takion_version_override = takion_version_override;
    request.skip_account_attributes_check = skip_account_attributes_check;
    request.stop = stop;
    request.cleared = cleared;
    return request;
  }
};

}  // namespace test_support::play
