#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/links/cloud/server_reservation.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace shared = libchiaking::progress;

class Recorder final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&, remote_play::FailureStage stage,
                   std::string_view text) override {
    seen.emplace_back(stage, std::string{text});
  }

  std::vector<std::pair<remote_play::FailureStage, std::string>> seen;
};

class TypedRecorder final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress& progress,
                   remote_play::FailureStage, std::string_view) override {
    typed.push_back(progress);
  }

  std::vector<play::LaunchProgress> typed;
};

TEST(LaunchReport, NumbersStepsAgainstTheirTotal) {
  Recorder recorder;
  const play::LaunchReport progress{
      recorder, libchiaking::catalog::ServiceGeneration::old_service};
  progress.step(play::LaunchActivity::reserving_server,
                remote_play::FailureStage::cloud_lock, "Locking Session", 7,
                play::ServerReservation::progress_steps);
  progress.step(play::LaunchActivity::resolving_title,
                remote_play::FailureStage::cloud_title, "Resolving Game", 2,
                play::OldServiceLaunch::progress_steps);
  progress.say(play::LaunchActivity::clearing_with_catalog,
               remote_play::FailureStage::cloud_launch_check,
               "Checking PlayStation Plus Premium and title streaming");
  ASSERT_EQ(recorder.seen.size(), std::size_t{3});
  EXPECT_EQ(recorder.seen[0].first, remote_play::FailureStage::cloud_lock);
  EXPECT_EQ(recorder.seen[0].second, "Locking Session - Step 7 of 10");
  EXPECT_EQ(recorder.seen[1].first, remote_play::FailureStage::cloud_title);
  EXPECT_EQ(recorder.seen[1].second, "Resolving Game - Step 2 of 5");
  EXPECT_EQ(recorder.seen[2].first,
            remote_play::FailureStage::cloud_launch_check);
  EXPECT_EQ(recorder.seen[2].second,
            "Checking PlayStation Plus Premium and title streaming");
}

TEST(LaunchReport, WritesAStepAsItsTextAndPosition) {
  EXPECT_EQ(play::LaunchReport::step_text("Getting Client IDs", 1, 10),
            "Getting Client IDs - Step 1 of 10");
}

TEST(LaunchReport, KeepsOneLessThanTheCapacityOfABoundedText) {
  Recorder recorder;
  const play::LaunchReport progress{
      recorder, libchiaking::catalog::ServiceGeneration::old_service};
  const std::string name(200, 'd');
  progress.step(play::LaunchActivity::measuring_datacenters,
                remote_play::FailureStage::cloud_datacenter,
                "Selecting Datacenter (" + name + ")", 9,
                play::ServerReservation::progress_steps,
                play::ServerReservation::datacenter_progress_text_limit);
  progress.say(play::LaunchActivity::allocating_server,
               remote_play::FailureStage::cloud_allocate,
               "Allocating streaming slot - Attempt " + name,
               play::ServerReservation::progress_text_limit);
  ASSERT_EQ(recorder.seen.size(), std::size_t{2});
  EXPECT_EQ(recorder.seen[0].second,
            ("Selecting Datacenter (" + name).substr(0, 95));
  EXPECT_EQ(recorder.seen[1].second,
            ("Allocating streaming slot - Attempt " + name).substr(0, 159));
}

TEST(LaunchReport, ReportsNowhereWithoutAnObserver) {
  const play::LaunchReport progress;
  progress.say(play::LaunchActivity::clearing_with_catalog,
               remote_play::FailureStage::cloud_title, "ignored");
  progress.step(play::LaunchActivity::reserving_server,
                remote_play::FailureStage::cloud_title, "ignored", 1, 5);
}

class LaunchReportTyped : public ::testing::Test {
 protected:
  using Activity = play::LaunchActivity;
  using Report = play::LaunchReport;

  void SetUp() override {
    const Report report{recorder,
                        libchiaking::catalog::ServiceGeneration::ps5_cloud};
    const auto stage = remote_play::FailureStage::cloud_datacenter;
    report.say(Activity::measuring_datacenters, stage, "measuring",
               Report::measured(0, 3, 8, 10));
    report.say(Activity::measuring_datacenters, stage, "measuring",
               Report::measured(3, 3, 8, 10));
    report.say(Activity::measuring_datacenters, stage, "measuring",
               Report::measured(4, 3, 8, 10));
    report.say(
        Activity::reserving_server, stage, "waiting",
        Report::waiting(shared::WaitReason::previous_session_closing, 2, 10));
    report.say(Activity::reserving_server, stage, "waiting",
               Report::waiting(shared::WaitReason::network_unreachable, 0, -4));
    report.say(
        Activity::allocating_server, stage, "waiting",
        Report::waiting(shared::WaitReason::previous_session_closing, 1, 10));
    ASSERT_EQ(recorder.typed.size(), std::size_t{4});
  }

  TypedRecorder recorder;
};

TEST_F(LaunchReportTyped, ReportsMeasuredCountsWithTheirStep) {
  EXPECT_EQ(recorder.typed[0].measured(), (shared::Position{0, 3}));
  EXPECT_EQ(recorder.typed[0].step(), (shared::Position{8, 10}));
  EXPECT_FALSE(recorder.typed[0].wait());
  EXPECT_EQ(recorder.typed[1].measured(), (shared::Position{3, 3}));
}

TEST_F(LaunchReportTyped, ReportsAWaitWithItsAttempt) {
  EXPECT_EQ(recorder.typed[2].wait(),
            (shared::Wait{shared::WaitReason::previous_session_closing,
                          std::chrono::seconds{10}}));
  EXPECT_EQ(recorder.typed[2].attempt(), 2U);
  EXPECT_FALSE(recorder.typed[2].measured());
}

TEST_F(LaunchReportTyped, ClampsANegativeWaitAndLeavesOutAZeroAttempt) {
  EXPECT_EQ(recorder.typed[3].wait(),
            (shared::Wait{shared::WaitReason::network_unreachable,
                          std::chrono::seconds{0}}));
  EXPECT_FALSE(recorder.typed[3].attempt());
}

TEST_F(LaunchReportTyped, HasNoDatacenterChoiceWaitByDefault) {
  EXPECT_FALSE(recorder.datacenter_choice_wait());
}

TEST(LaunchObserver, DeclinesTheDatacenterChoiceByDefault) {
  Recorder recorder;
  EXPECT_FALSE(recorder.selects_datacenter_at_launch());
  EXPECT_EQ(recorder.await_datacenter_choice(platform::StopToken{}).kind,
            play::LaunchChoice::Kind::timed_out);
  EXPECT_EQ(recorder
                .await_datacenter_choice(platform::StopToken{
                    std::make_shared<test_support::Stopped>()})
                .kind,
            play::LaunchChoice::Kind::cancelled);
}

}  // namespace
}  // namespace cloud_test
