#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>

namespace play_test {
namespace {

namespace progress = libchiaking::progress;
using libchiaking::catalog::ServiceGeneration;
using libchiaking::play::LaunchActivity;
using libchiaking::play::LaunchProgress;
using progress::Position;
using progress::Status;
using progress::Wait;
using progress::WaitReason;
using Activity = LaunchActivity;

class LaunchProgressStatus : public ::testing::Test {
 protected:
  static constexpr auto ps5 = ServiceGeneration::ps5_cloud;

  static constexpr std::array all_reasons{
      WaitReason::previous_session_closing, WaitReason::server_queue,
      WaitReason::data_migration,           WaitReason::datacenter_choice,
      WaitReason::title_not_ready,          WaitReason::network_unreachable,
      WaitReason::console_waking,           WaitReason::console_joining,
      WaitReason::console_answering,        WaitReason::connection_retry,
      WaitReason::recent_cloud_session,
  };

  static constexpr std::array all_activities{
      Activity::clearing_with_catalog,
      Activity::checking_device,
      Activity::opening_title_session,
      Activity::resolving_title,
      Activity::taking_free_licence,
      Activity::opening_account_session,
      Activity::reserving_server,
      Activity::measuring_datacenters,
      Activity::waiting_for_datacenter_choice,
      Activity::allocating_server,
      Activity::migrating_data,
      Activity::connecting_to_server,
      Activity::measuring_bandwidth,
      Activity::loading_library,
      Activity::checking_membership,
      Activity::checking_cloud_streaming,
      Activity::checking_ownership,
      Activity::checking_entitlement,
      Activity::resolving_cloud_game,
      Activity::checking_exclusions,
      Activity::checking_release,
  };

  static auto allowed(LaunchActivity activity, WaitReason reason) -> bool {
    switch (reason) {
      case WaitReason::previous_session_closing:
      case WaitReason::recent_cloud_session:
      case WaitReason::title_not_ready:
      case WaitReason::connection_retry:
        return activity == Activity::reserving_server;
      case WaitReason::server_queue:
        return activity == Activity::allocating_server;
      case WaitReason::data_migration:
        return activity == Activity::migrating_data;
      case WaitReason::datacenter_choice:
        return activity == Activity::waiting_for_datacenter_choice;
      case WaitReason::network_unreachable:
        return activity == Activity::reserving_server ||
               activity == Activity::allocating_server;
      default:
        return false;
    }
  }

  template <class Reported>
  static auto rendered(const Reported& reported) -> Status {
    return reported.status();
  }
};

TEST_F(LaunchProgressStatus, WaitsOnlyInTheActivityThatWaits) {
  for (const auto activity : all_activities) {
    if (!LaunchProgress::belongs_to(activity, ps5)) continue;
    for (const auto reason : all_reasons) {
      SCOPED_TRACE(testing::PrintToString(activity));
      SCOPED_TRACE(testing::PrintToString(reason));
      const auto made = LaunchProgress::at(
          ps5, activity, {.wait = Wait{reason, std::chrono::seconds{10}}});
      EXPECT_EQ(made.has_value(), allowed(activity, reason));
      EXPECT_EQ(LaunchProgress::waits_for(activity, reason),
                allowed(activity, reason));
      if (made) {
        EXPECT_EQ(made->wait(), (Wait{reason, std::chrono::seconds{10}}));
        EXPECT_TRUE(made->status().is_waiting());
        EXPECT_EQ(made->status().wait, made->wait());
      }
    }
  }
  EXPECT_FALSE(LaunchProgress::at(
      ServiceGeneration::old_service, Activity::resolving_title,
      {.wait = Wait{WaitReason::network_unreachable}}));
}

TEST_F(LaunchProgressStatus, IsBuiltFromAPlainStep) {
  const auto plain = LaunchProgress::at(ps5, Activity::reserving_server,
                                        {.step = Position{7, 10}});
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain->wait());
  EXPECT_FALSE(plain->elapsed());
  EXPECT_FALSE(plain->measured());
  EXPECT_EQ(plain->status(),
            (Status{Position{7, 10}, 1, std::nullopt, std::nullopt}));
}

TEST_F(LaunchProgressStatus, CarriesTheAttemptAndWait) {
  const Wait closing{WaitReason::previous_session_closing,
                     std::chrono::seconds{10}};
  const auto held = LaunchProgress::at(ps5, Activity::reserving_server,
                                       {.attempt = 2, .wait = closing});
  ASSERT_TRUE(held);
  EXPECT_EQ(held->attempt(), 2U);
  EXPECT_EQ(held->wait(), closing);
  EXPECT_EQ(held->status(), (Status{std::nullopt, 2, std::nullopt, closing}));
  EXPECT_EQ(rendered(*held), held->status());
}

TEST_F(LaunchProgressStatus, AddsTheElapsedTime) {
  const Wait closing{WaitReason::previous_session_closing,
                     std::chrono::seconds{10}};
  const auto held = LaunchProgress::at(ps5, Activity::reserving_server,
                                       {.attempt = 2, .wait = closing});
  ASSERT_TRUE(held);
  const auto timed = held->after(std::chrono::seconds{23});
  EXPECT_EQ(timed.elapsed(), std::chrono::microseconds{23'000'000});
  EXPECT_EQ(timed.wait(), closing);
  EXPECT_EQ(timed.activity(), held->activity());
  EXPECT_EQ(timed.attempt(), held->attempt());
  EXPECT_NE(timed, *held);
  EXPECT_EQ(timed.status(),
            (Status{std::nullopt, 2, std::chrono::seconds{23}, closing}));
  EXPECT_EQ(timed, held->after(std::chrono::seconds{23}));
}

TEST_F(LaunchProgressStatus, TellsAnImmediateRetryFromAnUnknownOne) {
  const auto now =
      LaunchProgress::at(ps5, Activity::reserving_server,
                         {.attempt = 2,
                          .wait = Wait{WaitReason::title_not_ready,
                                       std::chrono::milliseconds{0}}});
  ASSERT_TRUE(now);
  ASSERT_TRUE(now->wait());
  EXPECT_EQ(now->wait()->next_try_in, std::chrono::milliseconds{0});
  const auto unknown =
      LaunchProgress::at(ps5, Activity::waiting_for_datacenter_choice,
                         {.wait = Wait{WaitReason::datacenter_choice}});
  ASSERT_TRUE(unknown);
  ASSERT_TRUE(unknown->wait());
  EXPECT_FALSE(unknown->wait()->next_try_in);
}

TEST_F(LaunchProgressStatus, EstimatesTheWaitOnlyForALiveServerQueueWait) {
  const auto queue = LaunchProgress::at(
      ps5, Activity::allocating_server,
      {.wait_time_estimate_seconds = 245,
       .wait = Wait{WaitReason::server_queue, std::chrono::seconds{15}}});
  ASSERT_TRUE(queue);
  EXPECT_EQ(queue->wait_time_estimate_seconds(), 245U);
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::allocating_server,
                                  {.wait_time_estimate_seconds = 245}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::migrating_data,
                                  {.wait_time_estimate_seconds = 245,
                                   .wait = Wait{WaitReason::data_migration}}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::allocating_server,
                                  {.wait_time_estimate_seconds = 0,
                                   .wait = Wait{WaitReason::server_queue}}));
}

TEST_F(LaunchProgressStatus, CountsMeasuredDatacentersOnlyWhileMeasuring) {
  const auto none = LaunchProgress::at(ps5, Activity::measuring_datacenters,
                                       {.measured = Position{0, 3}});
  ASSERT_TRUE(none);
  EXPECT_EQ(none->measured(), (Position{0, 3}));
  EXPECT_FALSE(none->step());
  const auto all =
      LaunchProgress::at(ps5, Activity::measuring_datacenters,
                         {.step = Position{8, 10}, .measured = Position{3, 3}});
  ASSERT_TRUE(all);
  EXPECT_EQ(all->measured(), (Position{3, 3}));
  EXPECT_EQ(all->step(), (Position{8, 10}));
  EXPECT_EQ(all->status().position, (Position{8, 10}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::measuring_datacenters,
                                  {.measured = Position{4, 3}}));
  EXPECT_FALSE(LaunchProgress::at(ps5, Activity::allocating_server,
                                  {.measured = Position{1, 3}}));
  EXPECT_NE(*none, *all);
}

}  // namespace
}  // namespace play_test
