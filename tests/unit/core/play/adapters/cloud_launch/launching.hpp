#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "tests/support/random.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/waiter.hpp"

#include <libchiaking/detail/play/cloud_launch.hpp>
#include <libchiaking/platform/net/default.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::cloud_launches {

using test_support::play::Title;

using test_support::FakeWaiter;
using test_support::LiveProviders;
using test_support::play::LaunchCase;
using test_support::play::RecordingListener;
using test_support::play::ScriptedClearance;
using test_support::play::ScriptedCloudPs;
using test_support::play::SignedInPlayer;

class FixedClock final : public libchiaking::platform::ClockProvider {
 public:
  auto now_microseconds() const noexcept -> std::uint64_t override {
    return 1'000;
  }
};

class Launching {
 public:
  explicit Launching(LaunchCase launched,
                     libchiaking::play::Datacenters* datacenters = nullptr)
      : launched_(std::move(launched)),
        clearance_(launched_.clearance()),
        core_({ps_, network_, clock_, LiveProviders::threading(), random_, waiter_, clearance_,
               launched_.utc_offset, datacenters},
              launched_.request(), listener_) {}

  Launching(const Launching&) = delete;
  auto operator=(const Launching&) -> Launching& = delete;

  auto ps() -> ScriptedCloudPs& { return ps_; }
  auto waiter() const -> const FakeWaiter& { return waiter_; }
  auto clearance() -> ScriptedClearance& { return clearance_; }
  auto listener() const -> const RecordingListener& { return listener_; }
  auto core() -> libchiaking::play::CloudLaunch::Core& { return core_; }

 private:
  LaunchCase launched_;
  ScriptedCloudPs ps_;
  libchiaking::platform::DefaultNetworkProvider network_{LiveProviders::threading(), LiveProviders::steady()};
  FixedClock clock_;
  test_support::FailingRandom random_;
  FakeWaiter waiter_;
  ScriptedClearance clearance_;
  RecordingListener listener_;
  libchiaking::play::CloudLaunch::Core core_;
};

class LaunchTest : public ::testing::Test {
 protected:
  using Activity = libchiaking::play::LaunchActivity;
  using Kind = libchiaking::play::LaunchRefused::Kind;
  using Step = std::pair<std::uint32_t, std::uint32_t>;

  auto ps5_case() const -> LaunchCase {
    LaunchCase launched;
    launched.user = player_.user;
    return launched;
  }

  auto old_service_case(std::string_view held_as = {}) const -> LaunchCase {
    LaunchCase launched;
    launched.user = player_.user;
    launched.title =
        Title{.product = "UP0114-CUSA00001_00-FAKE0102TITLE000",
              .service = libchiaking::catalog::ServiceGeneration::old_service,
              .platform = libchiaking::catalog::Platform::ps4,
              .held_as = held_as,
              .store = "en-GB"};
    return launched;
  }

  auto player() const -> const SignedInPlayer& { return player_; }

  static auto is_old_service_only(Activity activity) -> bool {
    return activity == Activity::opening_title_session ||
           activity == Activity::resolving_title ||
           activity == Activity::taking_free_licence ||
           activity == Activity::opening_account_session;
  }

  static auto step_of(const libchiaking::play::LaunchProgress& progress)
      -> Step {
    const auto step = progress.step();
    return step ? Step{step->number, step->of} : Step{0U, 0U};
  }

  static auto waits_of(
      const std::vector<libchiaking::play::LaunchProgress>& typed)
      -> std::vector<libchiaking::play::LaunchProgress> {
    std::vector<libchiaking::play::LaunchProgress> waits;
    std::ranges::copy_if(
        typed, std::back_inserter(waits),
        [](const auto& progress) { return progress.wait().has_value(); });
    return waits;
  }

  static auto noted(const Launching& launching, std::string_view needle)
      -> bool {
    return std::ranges::any_of(launching.listener().notes,
                               [needle](const std::string& note) {
                                 return note.find(needle) != std::string::npos;
                               });
  }

  static auto named(std::string_view name)
      -> libchiaking::play::DatacenterName {
    auto parsed = libchiaking::play::DatacenterName::parse(name);
    EXPECT_TRUE(parsed) << name;
    return std::move(parsed).value();
  }

  static auto measured(std::string_view name, int rtt_ms,
                       std::int64_t at_ms = 1000)
      -> libchiaking::play::DatacenterMeasurement {
    auto measurement = libchiaking::play::DatacenterMeasurement::create(
        named(name), std::chrono::milliseconds{rtt_ms},
        libchiaking::play::DatacenterMeasurement::Time{
            std::chrono::milliseconds{at_ms}});
    EXPECT_TRUE(measurement) << name;
    return std::move(measurement).value();
  }

 private:
  SignedInPlayer player_;
};

}  // namespace play_test::cloud_launches
