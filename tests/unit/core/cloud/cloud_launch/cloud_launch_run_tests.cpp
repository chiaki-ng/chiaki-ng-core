#include "src/core/users/server_time.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/cloud/cloud_launch/provision_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {
namespace {

namespace catalog = libchiaking::catalog;
using Stage = gk::Error::Stage;
using Refused = play::LaunchRefused::Kind;
using Texts = std::vector<std::string>;

class StopWhileRequesting final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    flag->stopped = true;
    request_saw_stop = request_saw_stop || stop.stop_requested();
    platform::HttpResponse response;
    response.status_code = 204;
    return response;
  }

  std::shared_ptr<test_support::StopState> flag;
  unsigned calls{};
  bool request_saw_stop{};
};

class CloudLaunchRun : public ::testing::Test, protected ProvisionRig {
 protected:
  static constexpr std::int64_t release_ms = 1'791'273'600'000;
  static constexpr std::string_view date = "Tue, 06 Oct 2026 08:00:00 GMT";

  void SetUp() override { config.user = player.user; }

  static auto at(std::int64_t milliseconds) -> catalog::ReleaseDate {
    return catalog::ReleaseDate::at(
        catalog::ReleaseDate::Time{std::chrono::milliseconds{milliseconds}});
  }

  static auto legacy_title(std::string_view held_as)
      -> test_support::play::Title {
    return test_support::play::Title{
        .product = "EP0000-CUSA12345_00-TITLE",
        .service = catalog::ServiceGeneration::old_service,
        .platform = catalog::Platform::ps4,
        .held_as = held_as,
        .store = "en-GB"};
  }

  auto gated(catalog::ReleaseDate release, std::string_view server_date)
      -> bool {
    test_support::FakeClock monotonic{5'000'000};
    libchiaking::users::ServerTime trusted{monotonic};
    GatewayProvider gated_provider;
    gated_provider.preflight_expected = true;
    gated_provider.server_date = std::string{server_date};
    auto clearance = config.clearance();
    clearance.refusal_at_clear = [&] {
      return catalog::LaunchRefusal::of_release(release, trusted.now());
    };
    const auto gated_result = provision_cleared(
        config, gated_provider, clearance, play::LaunchObserver::silent(),
        &trusted.server_clock());
    EXPECT_EQ(gated_provider.preflight_calls, 1U);
    EXPECT_EQ(clearance.clears, 1);
    EXPECT_TRUE(clearance.remembered.empty());
    if (gated_result) {
      EXPECT_EQ(gated_provider.calls, 10U);
      return false;
    }
    EXPECT_EQ(gated_provider.calls, 0U);
    const auto* unmet = gated_result.error().as<play::LaunchFailure::Unmet>();
    EXPECT_NE(unmet, nullptr);
    if (unmet != nullptr) {
      EXPECT_EQ(unmet->precondition(),
                play::LaunchFailure::Precondition::game_not_released);
    }
    EXPECT_EQ(gated_result.error().http_status(), 0);
    EXPECT_EQ(play::LinkFailure::unlaunched(gated_result.error()).describe(),
              "the game has not been released yet");
    EXPECT_EQ(gated_result.error().refused().kind(), Refused::not_cleared);
    return true;
  }

  test_support::play::SignedInPlayer player{
      test_support::play::SignedInPlayer::saved(
          "synthetic-token",
          "000000070041008042424242424242424242424242424242")};
  test_support::play::LaunchCase config;
};

TEST_F(CloudLaunchRun, IsCancelledByAStopRaisedDuringTheCheck) {
  auto flag = std::make_shared<test_support::StopState>();
  config.stop = platform::StopToken{flag};
  StopWhileRequesting provider_stop;
  provider_stop.flag = flag;
  EXPECT_FALSE(provision(config, provider_stop));
  EXPECT_EQ(provider_stop.calls, 1U);
  EXPECT_TRUE(provider_stop.request_saw_stop);
}

TEST_F(CloudLaunchRun, ProvisionsAfterTheDeviceCheck) {
  GatewayProvider public_success;
  public_success.preflight_expected = true;
  const auto random_before = random.calls;
  const auto result = provision(config, public_success);
  ASSERT_TRUE(result);
  EXPECT_EQ(public_success.preflight_calls, 1U);
  EXPECT_EQ(public_success.calls, 10U);
  EXPECT_EQ(random.calls, random_before);
  EXPECT_EQ(result.value().datacenter.selected_port, 8975);
}

TEST_F(CloudLaunchRun, ReportsTheStatusOfAFailedGatewayRequest) {
  for (const unsigned failing_call : {2U, 3U, 4U, 6U, 7U, 10U}) {
    SCOPED_TRACE(failing_call);
    GatewayProvider http_failed;
    http_failed.preflight_expected = true;
    http_failed.fail_at = failing_call;
    const auto result = provision(config, http_failed);
    ASSERT_FALSE(result);
    EXPECT_EQ(http_failed.calls, failing_call);
    EXPECT_EQ(result.error().http_status(), 500);
    EXPECT_FALSE(result.error().best_rtt_ms());
    EXPECT_EQ(result.error().detail(),
              failing_call == 3 ? R"({"sessionId":"synthetic-session"})" : "");
  }
}

TEST_F(CloudLaunchRun, AsksToSignInAgainWhenTheDeviceCheckIsRefused) {
  GatewayProvider refused;
  refused.preflight_expected = true;
  refused.reject_preflight = true;
  const auto result = provision(config, refused);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().http_status(), 403);
  EXPECT_EQ(result.error().refused().kind(), Refused::sign_in_expired);
}

TEST_F(CloudLaunchRun, NamesAStoredDatacenterThatIsGone) {
  GatewayProvider stored_unavailable;
  stored_unavailable.preflight_expected = true;
  config.datacenter = test_support::play::LaunchCase::named("missing");
  const auto result = provision(config, stored_unavailable);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().http_status(), 0);
  EXPECT_EQ(result.error().refused().kind(), Refused::datacenter_unavailable);
  ASSERT_TRUE(result.error().refused().datacenter());
  EXPECT_EQ(*result.error().refused().datacenter(), "missing");
}

TEST_F(CloudLaunchRun, LeavesNoDetailForALongGoneDatacenter) {
  GatewayProvider long_unavailable;
  long_unavailable.preflight_expected = true;
  config.datacenter =
      test_support::play::LaunchCase::named(std::string(200, 'd'));
  const auto result = provision(config, long_unavailable);
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().detail().empty());
}

TEST_F(CloudLaunchRun, ReadsTheReleaseGateDate) {
  EXPECT_EQ(libchiaking::psn::ServerClock::parse_http_date(date), release_ms);
}

TEST_F(CloudLaunchRun, GatesAGameNotYetReleasedByPsTime) {
  EXPECT_TRUE(gated(at(release_ms + 1), date));
  EXPECT_TRUE(gated(at(release_ms), date));
}

TEST_F(CloudLaunchRun, LaunchesAGameReleasedByPsTime) {
  EXPECT_FALSE(gated(at(release_ms - 86'400'000), date));
}

TEST_F(CloudLaunchRun, GatesAnUnannouncedGame) {
  EXPECT_TRUE(gated(catalog::ReleaseDate::unannounced(), date));
}

TEST_F(CloudLaunchRun, GatesADatedGameWithoutPsTime) {
  EXPECT_TRUE(gated(at(release_ms - 86'400'000), ""));
}

TEST_F(CloudLaunchRun, LaunchesAnUnrestrictedGameWithoutPsTime) {
  EXPECT_FALSE(gated(catalog::ReleaseDate::unrestricted(), ""));
}

TEST_F(CloudLaunchRun, RefusesANoBitrateBeforeAnyRequest) {
  config.bitrate_kbps = 0;
  GatewayProvider invalid_spec;
  invalid_spec.preflight_expected = true;
  const auto random_before = random.calls;
  const auto result = provision(config, invalid_spec);
  ASSERT_FALSE(result);
  EXPECT_EQ(invalid_spec.preflight_calls, 0U);
  EXPECT_EQ(invalid_spec.calls, 0U);
  EXPECT_EQ(random.calls, random_before);
  const auto* unmet = result.error().as<play::LaunchFailure::Unmet>();
  ASSERT_NE(unmet, nullptr);
  EXPECT_EQ(unmet->precondition(),
            play::LaunchFailure::Precondition::invalid_bitrate);
}

TEST_F(CloudLaunchRun, ReportsARefusedDeviceCheckAtTheIdentityStage) {
  GatewayProvider rejected_preflight;
  rejected_preflight.preflight_expected = true;
  rejected_preflight.reject_preflight = true;
  const auto result = provision(config, rejected_preflight);
  ASSERT_FALSE(result);
  EXPECT_EQ(rejected_preflight.preflight_calls, 1U);
  EXPECT_EQ(rejected_preflight.calls, 0U);
  const auto* reservation =
      result.error().as<play::LaunchFailure::ReservationFailed>();
  ASSERT_NE(reservation, nullptr);
  EXPECT_EQ(reservation->error().stage(), Stage::identity);
  EXPECT_EQ(reservation->error().detail(),
            static_cast<std::uint32_t>(gk::Error::IdentityDetail::rejected));
}

class CloudLaunchRetry : public CloudLaunchRun {
 protected:
  void SetUp() override {
    CloudLaunchRun::SetUp();
    config.title = legacy_title("OWNED");
  }

  static auto title_steps() -> Texts {
    return {"Authorizing - Step 4 of 5",    "Creating Session - Step 5 of 5",
            "Cloud Auth - Step 1 of 5",     "Cloud Auth - Step 1 of 5",
            "Resolving Game - Step 2 of 5", "Checking License - Step 3 of 5",
            "Authorizing - Step 4 of 5",    "Creating Session - Step 5 of 5"};
  }

  static auto starts_with_the_checks(const TitleProgress& progress)
      -> testing::AssertionResult {
    if (progress.gk_texts.size() <= 1)
      return testing::AssertionFailure()
             << progress.gk_texts.size() << " gateway steps were reported";
    if (progress.gk_texts[0] !=
        "Checking PlayStation Plus Premium and title streaming")
      return testing::AssertionFailure()
             << "began with " << progress.gk_texts[0];
    if (progress.gk_texts[1] != "Getting Client IDs - Step 1 of 10")
      return testing::AssertionFailure()
             << "continued with " << progress.gk_texts[1];
    return testing::AssertionSuccess();
  }

  RetryProvider retry;
  TitleProgress progress;
};

TEST_F(CloudLaunchRetry, ResolvesTheTitleWhenTheHeldLicenceIsRefused) {
  const auto random_calls = random.calls;
  auto clearance = config.clearance();
  const auto retried = provision_cleared(config, retry, clearance, progress);
  ASSERT_TRUE(retried);
  EXPECT_EQ(clearance.clears, 1);
  EXPECT_TRUE(clearance.remembered.empty());
  EXPECT_EQ(progress.title_texts, title_steps());
  EXPECT_TRUE(starts_with_the_checks(progress));
  EXPECT_EQ(retry.preflights, 1U);
  EXPECT_EQ(random.calls, random_calls);
  EXPECT_EQ(retry.lookups, 1U);
  EXPECT_EQ(retry.starts, 2U);
  EXPECT_EQ(retry.account_sessions, 3U);
  EXPECT_EQ(retried.value().service, CloudService::legacy_cloud_stream);
}

TEST_F(CloudLaunchRetry, RemembersARefusalThatPersistsAfterResolving) {
  retry.reject_again = true;
  const auto random_calls = random.calls;
  auto clearance = config.clearance();
  const auto retried = provision_cleared(config, retry, clearance, progress);
  ASSERT_FALSE(retried);
  EXPECT_EQ(clearance.clears, 1);
  ASSERT_EQ(clearance.remembered.size(), 1U);
  EXPECT_EQ(clearance.remembered.front().product, "EP0000-CUSA12345_00-TITLE");
  EXPECT_EQ(clearance.remembered.front().kind,
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_EQ(retried.error().refused().kind(), Refused::refused_by_service);
  EXPECT_TRUE(retried.error().refused().is_reported_to_catalog());
  EXPECT_EQ(progress.title_texts, title_steps());
  EXPECT_TRUE(starts_with_the_checks(progress));
  EXPECT_EQ(retry.preflights, 1U);
  EXPECT_EQ(random.calls, random_calls);
  EXPECT_EQ(retry.lookups, 1U);
  EXPECT_EQ(retry.starts, 2U);
  EXPECT_EQ(retry.account_sessions, 3U);
}

TEST_F(CloudLaunchRetry, DoesNotRetryAnUnrelatedRefusal) {
  retry.unrelated_failure = true;
  const auto random_calls = random.calls;
  auto clearance = config.clearance();
  const auto retried = provision_cleared(config, retry, clearance, progress);
  ASSERT_FALSE(retried);
  EXPECT_EQ(clearance.clears, 1);
  EXPECT_TRUE(clearance.remembered.empty());
  EXPECT_EQ(retried.error().refused().kind(), Refused::unavailable);
  EXPECT_TRUE(starts_with_the_checks(progress));
  EXPECT_EQ(retry.preflights, 1U);
  EXPECT_EQ(random.calls, random_calls);
  EXPECT_EQ(retry.lookups, 0U);
  EXPECT_EQ(retry.starts, 1U);
  EXPECT_EQ(retry.account_sessions, 1U);
}

TEST_F(CloudLaunchRetry, ResolvesAnUnheldTitleOnce) {
  config.title = legacy_title({});
  EXPECT_TRUE(provision(config, retry));
  EXPECT_EQ(retry.lookups, 1U);
  EXPECT_EQ(retry.starts, 1U);
  EXPECT_EQ(retry.account_sessions, 2U);
}

TEST_F(CloudLaunchRetry, AsksForPrivacySettingsTheServiceNeeds) {
  config.title = legacy_title({});
  retry.reject_attributes = true;
  const auto rejected = provision(config, retry);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(retry.attribute_checks, 1U);
  const auto* resolve =
      rejected.error().as<play::LaunchFailure::TitleRefused>();
  ASSERT_NE(resolve, nullptr);
  EXPECT_EQ(resolve->error().cause(),
            play::OldServiceLaunch::Error::Cause::privacy_required);
  EXPECT_TRUE(rejected.error().upgrade_url().starts_with(
      "https://id.sonyentertainmentnetwork.com/id/upgrade_account_ca/"))
      << rejected.error().upgrade_url();
  EXPECT_NE(rejected.error().upgrade_url().find(
                "missing_elements=PRIVACY_SETTING_BROADCAST"),
            std::string::npos);
  EXPECT_EQ(play::LinkFailure::unlaunched(rejected.error()).action_url(),
            rejected.error().upgrade_url());
  const auto typed = rejected.error().refused();
  EXPECT_EQ(typed.kind(), Refused::privacy_settings);
  EXPECT_EQ(typed.upgrade_url(), rejected.error().upgrade_url());
}

TEST_F(CloudLaunchRetry, SkipsTheAccountCheckWhenAsked) {
  config.title = legacy_title({});
  retry.reject_attributes = true;
  config.skip_account_attributes_check = true;
  EXPECT_TRUE(provision(config, retry));
  EXPECT_EQ(retry.attribute_checks, 0U);
}

TEST_F(CloudLaunchRetry, RemembersACloudStreamRefusalOfAnOwnedTitle) {
  config.title = test_support::play::Title{.product = "OWNED"};
  auto clearance = config.clearance();
  const auto rejected = provision_cleared(config, retry, clearance);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(retry.lookups, 0U);
  EXPECT_EQ(retry.starts, 2U);
  EXPECT_EQ(rejected.error().detail(), R"({"name":"noGameForEntitlementId"})");
  EXPECT_EQ(clearance.clears, 1);
  ASSERT_EQ(clearance.remembered.size(), 1U);
  EXPECT_EQ(clearance.remembered.front().product, "OWNED");
  EXPECT_EQ(clearance.remembered.front().kind,
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
  const auto typed = rejected.error().refused();
  EXPECT_EQ(typed.kind(), Refused::refused_by_service);
  EXPECT_TRUE(typed.is_reported_to_catalog());
  EXPECT_EQ(typed.ps_refusal(),
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
}

}  // namespace
}  // namespace cloud_test
