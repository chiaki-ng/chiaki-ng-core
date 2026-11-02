#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/psn/graphql/check.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_failure_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::catalog::LaunchRefusal;
using Unanswered = libchiaking::catalog::LaunchCheckFailure;
using link_failures::Failures;
using LegacyCloudStreamError = play::OldServiceLaunch::Error;
using SessionError = libchiaking::psn::gk::Error;
using Stage = remote_play::FailureStage;

TEST(LinkFailureInTheCloud, IsNotAServiceFailureWhenCancelled) {
  for (const auto stage :
       {SessionError::Stage::start, SessionError::Stage::oauth,
        SessionError::Stage::lock, SessionError::Stage::config,
        SessionError::Stage::client_ids}) {
    SCOPED_TRACE(testing::PrintToString(stage));
    auto failure =
        play::LinkFailure::unlaunched(play::LaunchFailure::reservation(
            SessionError{stage, SessionError::Reason::rejected}, {}));
    failure.cancel();
    const auto text =
        Failures::at(Failures::cloud_stream(), failure).sentence();
    EXPECT_TRUE(Failures::contains(text, "cancelled"));
    EXPECT_FALSE(Failures::contains(text, "cloud service request failed"));
  }
}

TEST(LinkFailureInTheCloud, InterpretsAnUnansweredLaunchCheck) {
  using libchiaking::psn::graphql::CheckStatus;
  const auto launch =
      play::LaunchFailure::not_cleared(LaunchRefusal::check_unavailable(
          Unanswered{Unanswered::Reason::rejected, 403}));
  const auto* not_cleared = launch.as<play::LaunchFailure::NotCleared>();
  ASSERT_NE(not_cleared, nullptr);
  EXPECT_EQ(not_cleared->unanswered(), CheckStatus::rejected);
  const auto detail = play::LinkFailure::unlaunched(launch);
  EXPECT_TRUE(Failures::contains(detail.sentence(), "Check status"));
}

class LinkFailureLegacyTitle : public ::testing::Test {
 protected:
  static auto title(LegacyCloudStreamError error) -> std::string {
    return Failures::at(
               Failures::legacy_cloud_stream(),
               play::LinkFailure::unlaunched(play::LaunchFailure::title(error)))
        .sentence();
  }

  static auto privacy(const std::vector<std::string>& missing)
      -> play::LinkFailure {
    return Failures::at(
        Failures::legacy_cloud_stream(),
        play::LinkFailure::unlaunched(play::LaunchFailure::title(
            LegacyCloudStreamError::privacy_required(400, missing))));
  }

  static auto unmet(play::LaunchFailure::Precondition cause) -> std::string {
    return Failures::at(Failures::legacy_cloud_stream(),
                        play::LinkFailure::unlaunched(
                            play::LaunchFailure::precondition(cause)))
        .sentence();
  }
};

TEST_F(LinkFailureLegacyTitle, NamesAGameThatWasNotFound) {
  const auto not_found = title(
      LegacyCloudStreamError{LegacyCloudStreamError::Stage::title,
                             LegacyCloudStreamError::Cause::not_found, 404});
  EXPECT_TRUE(Failures::contains(not_found, "Game not found"));
  EXPECT_TRUE(Failures::contains(
      not_found, "does not exist or is not available for cloud streaming"));
  EXPECT_FALSE(Failures::contains(not_found, "Game not free"));
}

TEST_F(LinkFailureLegacyTitle, NamesAGameThatIsNoLongerFree) {
  const auto not_free = title(
      LegacyCloudStreamError{LegacyCloudStreamError::Stage::checkout_preview,
                             LegacyCloudStreamError::Cause::not_free, 200});
  EXPECT_TRUE(Failures::contains(not_free, "Game not free"));
  EXPECT_TRUE(Failures::contains(not_free, "no longer free"));
  EXPECT_FALSE(Failures::contains(not_free, "Game not found"));
}

TEST_F(LinkFailureLegacyTitle, NamesAGameOutsideEveryPlan) {
  const auto outside = title(LegacyCloudStreamError{
      LegacyCloudStreamError::Stage::title,
      LegacyCloudStreamError::Cause::outside_streaming_plan, 200});
  EXPECT_TRUE(
      Failures::contains(outside, "not part of any PlayStation Plus plan"));
  EXPECT_FALSE(
      Failures::contains(outside, "check your PlayStation Plus subscription"));
}

TEST_F(LinkFailureLegacyTitle, NamesTheServiceAndStatusAtEveryStage) {
  for (auto stage : {LegacyCloudStreamError::Stage::anonymous_oauth,
                     LegacyCloudStreamError::Stage::anonymous_session,
                     LegacyCloudStreamError::Stage::title,
                     LegacyCloudStreamError::Stage::commerce_oauth,
                     LegacyCloudStreamError::Stage::account_attributes,
                     LegacyCloudStreamError::Stage::entitlement,
                     LegacyCloudStreamError::Stage::checkout_preview,
                     LegacyCloudStreamError::Stage::checkout,
                     LegacyCloudStreamError::Stage::account_session}) {
    SCOPED_TRACE(testing::PrintToString(stage));
    const auto failure = play::LinkFailure::unlaunched(
        play::LaunchFailure::title(LegacyCloudStreamError{
            stage, LegacyCloudStreamError::Cause::rejected, 403}));
    const auto text =
        Failures::at(Failures::legacy_cloud_stream(), failure).sentence();
    EXPECT_TRUE(text.starts_with("LegacyCloudStream: ")) << text;
    EXPECT_TRUE(Failures::contains(text, "HTTP 403"));
    EXPECT_EQ(failure.stage(), Stage::cloud_title);
  }
}

TEST_F(LinkFailureLegacyTitle, CarriesThePrivacyUpgradeUrl) {
  const auto failure = privacy({"PRIVACY_SETTING_BROADCAST"});
  EXPECT_TRUE(failure.action_url().starts_with(
      "https://id.sonyentertainmentnetwork.com/"))
      << failure.action_url();
  EXPECT_TRUE(
      Failures::contains(failure.action_url(), "PRIVACY_SETTING_BROADCAST"));
  EXPECT_FALSE(
      Failures::contains(failure.sentence(), "ACCOUNT_PRIVACY_SETTINGS"));
}

TEST_F(LinkFailureLegacyTitle, BoundsThePrivacyUpgradeUrl) {
  EXPECT_LE(privacy(std::vector<std::string>(40, std::string(500, 'E')))
                .end_reason()
                .action_url()
                .size(),
            play::EndReason::url_limit);
}

TEST_F(LinkFailureLegacyTitle, DescribesAPrivacyRefusalWithoutAUrl) {
  const auto bare = Failures::at(
      Failures::legacy_cloud_stream(),
      play::LinkFailure::unlaunched(
          play::LaunchFailure::title(LegacyCloudStreamError{
              LegacyCloudStreamError::Stage::account_attributes,
              LegacyCloudStreamError::Cause::privacy_required, 400})));
  EXPECT_TRUE(bare.action_url().empty());
  EXPECT_EQ(play::LinkStages::name(bare.stage()),
            "LegacyCloudStream title preparation");
  EXPECT_EQ(bare.describe(),
            "PlayStation account privacy settings need to be completed");
  EXPECT_EQ(play::LinkStages::name(Stage::media), "Media stream");
}

TEST_F(LinkFailureLegacyTitle, NamesAnUnmetPrecondition) {
  using P = play::LaunchFailure::Precondition;
  for (auto cause : {P::invalid_service, P::invalid_platform, P::invalid_npsso,
                     P::invalid_bitrate, P::invalid_spec, P::invalid_duid}) {
    SCOPED_TRACE(testing::PrintToString(cause));
    const auto text = unmet(cause);
    EXPECT_FALSE(Failures::contains(text, "Detail "));
    EXPECT_FALSE(Failures::contains(text, "cloud service request failed"));
  }
  EXPECT_TRUE(Failures::contains(unmet(P::invalid_duid), "DUID"));
  EXPECT_TRUE(Failures::contains(unmet(P::invalid_npsso), "npsso"));
}

class LinkFailureLaunchCheck : public ::testing::Test {
 protected:
  static auto describe(const LaunchRefusal& refusal, std::uint16_t http)
      -> std::string {
    const auto failure =
        Failures::at(Failures::cloud_stream(),
                     play::LinkFailure::unlaunched(
                         play::LaunchFailure::not_cleared(refusal)));
    EXPECT_EQ(failure.stage(), Stage::cloud_launch_check);
    EXPECT_EQ(failure.http_status(), http);
    EXPECT_TRUE(failure.message().empty());
    return failure.sentence();
  }
};

TEST_F(LinkFailureLaunchCheck, NamesTheTierItNeeds) {
  const auto premium = describe(
      LaunchRefusal::needs_premium(libchiaking::users::Tier::extra), 0);
  EXPECT_TRUE(premium.starts_with("CloudStream: Cloud launch checks"))
      << premium;
  EXPECT_TRUE(Failures::contains(premium, "PlayStation Plus Premium"));
}

TEST_F(LinkFailureLaunchCheck, NamesATitleThatCannotBeStreamed) {
  EXPECT_TRUE(Failures::contains(
      describe(LaunchRefusal::not_streamable("notice"), 0), "cloud streaming"));
}

TEST_F(LinkFailureLaunchCheck, NamesATitleMissingFromTheLibrary) {
  EXPECT_TRUE(Failures::contains(
      describe(LaunchRefusal::add_to_library(
                   libchiaking::catalog::StorefrontAnswer::LibraryOffer{
                       "SKU-1", {"ENT-1"}, false}),
               0),
      "library"));
}

TEST_F(LinkFailureLaunchCheck, NamesAnUnavailableCheck) {
  const auto unavailable = describe(LaunchRefusal::check_unavailable(Unanswered{
                                        Unanswered::Reason::unavailable, 500}),
                                    500);
  EXPECT_TRUE(Failures::contains(unavailable, "could not be completed"));
  EXPECT_TRUE(Failures::contains(unavailable, "Check status unavailable."));
  EXPECT_TRUE(Failures::contains(unavailable, "HTTP 500."));
}

class LinkFailureCloudSession : public ::testing::Test {
 protected:
  static auto failed(SessionError error) -> play::LinkFailure {
    return play::LinkFailure::unlaunched(
        play::LaunchFailure::reservation(std::move(error), {}));
  }
};

TEST_F(LinkFailureCloudSession, NamesTheCreationStage) {
  const auto failure = failed(
      SessionError{SessionError::Stage::start, SessionError::Reason::rejected});
  EXPECT_EQ(failure.stage(), Stage::cloud_start);
  EXPECT_TRUE(Failures::contains(failure.sentence(), "Cloud session creation"));
}

TEST_F(LinkFailureCloudSession, NamesAnExpiredAllocationDeadline) {
  const auto failure = failed(SessionError{SessionError::Stage::allocate,
                                           SessionError::Reason::timed_out});
  EXPECT_EQ(failure.stage(), Stage::cloud_allocate);
  EXPECT_TRUE(
      Failures::contains(failure.sentence(), "allocation deadline expired"));
}

TEST_F(LinkFailureCloudSession, NamesTheBestRttWhenNoDatacenterIsReachable) {
  const auto failure = failed(SessionError::no_reachable_datacenter(97));
  EXPECT_EQ(failure.stage(), Stage::cloud_datacenter);
  EXPECT_EQ(failure.http_status(), 0);
  EXPECT_TRUE(Failures::contains(
      failure.sentence(),
      "no datacenter passed the latency and reachability checks. Detail "
      "no_reachable_datacenter. Best measured RTT 97 ms."));
  EXPECT_FALSE(Failures::contains(
      failed(SessionError::no_reachable_datacenter(std::nullopt)).sentence(),
      "Best measured"));
}

TEST_F(LinkFailureCloudSession, NamesTheStatusOfARejectedAllocation) {
  const auto failure = failed(SessionError{
      SessionError::Stage::allocate, SessionError::Reason::rejected, 503});
  EXPECT_EQ(failure.stage(), Stage::cloud_allocate);
  EXPECT_EQ(failure.http_status(), 503);
  EXPECT_TRUE(Failures::contains(failure.sentence(), "HTTP 503"));
}

TEST_F(LinkFailureCloudSession, NamesARejectedSignInCheck) {
  const auto failure = failed(SessionError{SessionError::Stage::identity,
                                           SessionError::Reason::rejected});
  EXPECT_EQ(failure.stage(), Stage::cloud_identity);
  EXPECT_TRUE(Failures::contains(failure.sentence(), "sign-in check rejected"));
}

}  // namespace
}  // namespace play_test
