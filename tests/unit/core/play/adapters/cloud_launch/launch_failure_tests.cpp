#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace psn = libchiaking::psn;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Error = psn::gk::Error;
using Reason = Error::Reason;
using Stage = Error::Stage;
using Title = play::OldServiceLaunch::Error;
using Status = psn::graphql::CheckStatus;
using Cleared = catalog::LaunchRefusal::Kind;
using Kind = play::LaunchRefused::Kind;
using Unanswered = catalog::LaunchCheckFailure;
using UserReason = libchiaking::users::Unanswered::Reason;

class LaunchRefusalSource : public ::testing::Test {
 protected:
  static auto cleared(const catalog::LaunchRefusal& refusal)
      -> play::LaunchFailure {
    return play::LaunchFailure::not_cleared(refusal);
  }

  static auto refusal_of(const play::LaunchFailure& failure) -> Cleared {
    return failure.as<play::LaunchFailure::NotCleared>()->refusal().kind();
  }

  static auto unanswered(const play::LaunchFailure& failure)
      -> std::optional<Status> {
    return failure.as<play::LaunchFailure::NotCleared>()->unanswered();
  }

  static auto unanswered_for(Unanswered reason) -> std::optional<Status> {
    return unanswered(
        cleared(catalog::LaunchRefusal::check_unavailable(std::move(reason))));
  }

  static auto reservation(Error error, std::string_view datacenter = {})
      -> play::LaunchRefused {
    return play::LaunchFailure::reservation(std::move(error), datacenter)
        .refused();
  }
};

TEST_F(LaunchRefusalSource, NeedsPremiumNamesTheTierHeld) {
  const auto failure = cleared(
      catalog::LaunchRefusal::needs_premium(libchiaking::users::Tier::extra));
  EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
  ASSERT_TRUE(failure.refused().clearance());
  EXPECT_EQ(failure.refused().clearance()->tier_held(),
            libchiaking::users::Tier::extra);
  EXPECT_EQ(refusal_of(failure), Cleared::needs_premium);
  EXPECT_FALSE(unanswered(failure));
  EXPECT_EQ(failure.describe(),
            "cloud streaming requires an active PlayStation Plus Premium "
            "subscription");
  const auto detail = play::LinkFailure::unlaunched(failure);
  EXPECT_EQ(detail.stage(), remote_play::FailureStage::cloud_launch_check);
}

TEST_F(LaunchRefusalSource, NotStreamableKeepsTheStorefrontNotice) {
  const auto failure = cleared(
      catalog::LaunchRefusal::not_streamable("Streaming is not available"));
  EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
  EXPECT_FALSE(failure.refused().is_reported_to_catalog());
  EXPECT_FALSE(failure.refused().ps_refusal());
  EXPECT_EQ(refusal_of(failure), Cleared::not_streamable);
  EXPECT_TRUE(failure.detail().empty());
  ASSERT_TRUE(failure.refused().clearance());
  EXPECT_EQ(failure.refused().clearance()->notice(),
            "Streaming is not available");
}

TEST_F(LaunchRefusalSource, AddToLibraryKeepsTheOffer) {
  const auto failure = cleared(catalog::LaunchRefusal::add_to_library(
      catalog::StorefrontAnswer::LibraryOffer{
          "SKU-1", {"ENT-1", "ENT-2"}, false}));
  EXPECT_EQ(refusal_of(failure), Cleared::add_to_library);
  ASSERT_TRUE(failure.refused().clearance());
  ASSERT_TRUE(failure.refused().clearance()->library_offer());
  EXPECT_EQ(failure.refused().clearance()->library_offer()->sku_id, "SKU-1");
}

TEST_F(LaunchRefusalSource, BuyToStreamIsKept) {
  EXPECT_EQ(refusal_of(cleared(catalog::LaunchRefusal::buy_to_stream())),
            Cleared::buy_to_stream);
}

TEST_F(LaunchRefusalSource, EachCatalogRefusalHasItsOwnDescription) {
  const std::string generic{
      cleared(catalog::LaunchRefusal::check_unavailable(
                  Unanswered{Unanswered::Reason::unavailable}))
          .describe()};
  for (const auto& reread :
       {catalog::LaunchRefusal::invalid_title(),
        catalog::LaunchRefusal::title_taken_down(),
        catalog::LaunchRefusal::streaming_unavailable(),
        catalog::LaunchRefusal::purchase_needed(),
        catalog::LaunchRefusal::unexpected_entitlement_state(),
        catalog::LaunchRefusal::choose_a_game()}) {
    SCOPED_TRACE(static_cast<int>(reread.kind()));
    const auto failure = cleared(reread);
    EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
    EXPECT_EQ(refusal_of(failure), reread.kind());
    EXPECT_FALSE(unanswered(failure));
    EXPECT_FALSE(failure.describe().empty());
    EXPECT_NE(failure.describe(), generic);
  }
}

TEST_F(LaunchRefusalSource, ACatalogRefusedByServiceIsNotReportedAgain) {
  const auto failure = cleared(catalog::LaunchRefusal::refused_by_service(
      catalog::LaunchRefusal::Time{}));
  EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
  EXPECT_FALSE(failure.refused().is_reported_to_catalog());
  EXPECT_EQ(refusal_of(failure), Cleared::refused_by_service);
  EXPECT_FALSE(unanswered(failure));
}

TEST_F(LaunchRefusalSource, AnUnlistedTitleIsNotFound) {
  for (const auto& unlisted :
       {catalog::LaunchRefusal::not_listed(),
        catalog::LaunchRefusal::hidden(
            catalog::HideReason::tier_does_not_include)}) {
    SCOPED_TRACE(static_cast<int>(unlisted.kind()));
    const auto failure = cleared(unlisted);
    EXPECT_EQ(unanswered(failure), Status::not_found);
    EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
  }
}

TEST_F(LaunchRefusalSource, AnUnavailableCheckKeepsItsHttpStatus) {
  const auto failure = cleared(catalog::LaunchRefusal::check_unavailable(
      Unanswered{Unanswered::Reason::unavailable, 503}));
  EXPECT_EQ(refusal_of(failure), Cleared::check_unavailable);
  EXPECT_EQ(unanswered(failure), Status::unavailable);
  EXPECT_EQ(failure.http_status(), 503);
  const auto detail = play::LinkFailure::unlaunched(failure);
  EXPECT_EQ(detail.http_status(), 503);
  EXPECT_NE(detail.sentence().find(" Check status "), std::string::npos);
}

TEST_F(LaunchRefusalSource, EachUnansweredReasonHasACheckStatus) {
  EXPECT_EQ(unanswered_for(Unanswered{Unanswered::Reason::malformed}),
            Status::malformed);
  EXPECT_EQ(unanswered_for(Unanswered{Unanswered::Reason::rejected}),
            Status::rejected);
  EXPECT_EQ(unanswered_for(Unanswered{Unanswered::Reason::cancelled}),
            Status::cancelled);
  EXPECT_EQ(unanswered_for(Unanswered{
                Unanswered::Reason::user, 0,
                libchiaking::users::Unanswered{UserReason::signed_out}}),
            Status::no_session);
  EXPECT_EQ(unanswered_for(Unanswered{
                Unanswered::Reason::user, 0,
                libchiaking::users::Unanswered{UserReason::sign_in_expired}}),
            Status::auth_refused);
}

TEST_F(LaunchRefusalSource, AnUnreleasedGameIsAnUnmetPrecondition) {
  for (const auto& unreleased :
       {catalog::LaunchRefusal::unannounced(),
        catalog::LaunchRefusal::no_trusted_clock(),
        catalog::LaunchRefusal::not_released(
            catalog::LaunchRefusal::Time{std::chrono::hours{1}})}) {
    SCOPED_TRACE(static_cast<int>(unreleased.kind()));
    const auto failure = cleared(unreleased);
    ASSERT_TRUE(failure.as<play::LaunchFailure::Unmet>());
    EXPECT_EQ(failure.as<play::LaunchFailure::Unmet>()->precondition(),
              play::LaunchFailure::Precondition::game_not_released);
    EXPECT_EQ(failure.refused().kind(), Kind::not_cleared);
    ASSERT_TRUE(failure.refused().clearance());
    EXPECT_EQ(failure.refused().clearance()->kind(), unreleased.kind());
    const auto detail = play::LinkFailure::unlaunched(failure);
    EXPECT_EQ(detail.stage(), remote_play::FailureStage::cloud_provision);
    EXPECT_EQ(detail.describe(), "the game has not been released yet");
  }
}

TEST_F(LaunchRefusalSource, MissingPrivacySettingsCarryABoundedUpgradeUrl) {
  const std::vector<std::string> missing{"PRIVACY_SETTING_BROADCAST"};
  const auto titled =
      play::LaunchFailure::title(Title::privacy_required(400, missing));
  EXPECT_EQ(titled.refused().kind(), Kind::privacy_settings);
  EXPECT_EQ(titled.refused().upgrade_url(), titled.upgrade_url());
  EXPECT_TRUE(titled.refused().upgrade_url().starts_with(
      "https://id.sonyentertainmentnetwork.com/"));
  const std::vector<std::string> many(40, std::string(500, 'E'));
  EXPECT_LE(play::LaunchFailure::title(Title::privacy_required(400, many))
                .refused()
                .upgrade_url()
                .size(),
            play::LaunchRefused::url_limit);
  const auto bare = play::LaunchFailure::title(Title{
      Title::Stage::account_attributes, Title::Cause::privacy_required, 400});
  EXPECT_EQ(bare.refused().kind(), Kind::privacy_settings);
  EXPECT_TRUE(bare.refused().upgrade_url().empty());
  EXPECT_TRUE(bare.upgrade_url().empty());
  EXPECT_TRUE(bare.detail().empty());
}

TEST_F(LaunchRefusalSource, ALicenceThatIsNotFreeShowsOnlyAShortPrice) {
  auto titled = play::LaunchFailure::title(Title::not_free("$9.99"));
  EXPECT_EQ(titled.refused().kind(), Kind::licence_not_free);
  EXPECT_EQ(titled.refused().display_price(), "$9.99");
  titled = play::LaunchFailure::title(Title::not_free(std::string(300, '9')));
  EXPECT_EQ(titled.refused().kind(), Kind::licence_not_free);
  EXPECT_TRUE(titled.refused().display_price().empty());
}

TEST_F(LaunchRefusalSource, ARejectedMembershipComesFromCheckoutOrAuthorize) {
  const auto titled = play::LaunchFailure::title(
      Title{Title::Stage::checkout_preview, Title::Cause::subscription_required,
            403});
  EXPECT_EQ(titled.refused().kind(), Kind::membership_rejected);
  EXPECT_EQ(
      reservation(Error{Stage::authorize, Reason::psplus_rejected, 403}).kind(),
      Kind::membership_rejected);
}

TEST_F(LaunchRefusalSource, TitleErrorsAreNotFoundOrUnavailable) {
  auto titled = play::LaunchFailure::title(
      Title{Title::Stage::title, Title::Cause::not_found, 404});
  EXPECT_EQ(titled.refused().kind(), Kind::title_not_found);
  titled = play::LaunchFailure::title(
      Title{Title::Stage::checkout, Title::Cause::rejected, 500});
  EXPECT_EQ(titled.refused().kind(), Kind::unavailable);
  EXPECT_EQ(titled.refused().http_status(), 500);
}

TEST_F(LaunchRefusalSource, OnlyAServiceRefusalIsReportedToTheCatalog) {
  const auto no_game = play::LaunchFailure::refused_by_service(
      Error::rejected(Stage::start, Reason::entitlement_retry, 400, "body"),
      "dc1", catalog::StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_EQ(no_game.refused().kind(), Kind::refused_by_service);
  EXPECT_TRUE(no_game.refused().is_reported_to_catalog());
  EXPECT_EQ(no_game.detail(), "body");
  EXPECT_EQ(no_game.http_status(), 400);
  EXPECT_EQ(reservation(Error::rejected(Stage::start, Reason::entitlement_retry,
                                        400, "body"))
                .kind(),
            Kind::unavailable);
}

TEST_F(LaunchRefusalSource, NoReachableDatacenterKeepsTheBestRoundTrip) {
  auto unreachable = reservation(Error::no_reachable_datacenter(97));
  EXPECT_EQ(unreachable.kind(), Kind::no_reachable_datacenter);
  EXPECT_TRUE(unreachable.is_about_datacenters());
  EXPECT_EQ(unreachable.best_round_trip(), std::chrono::milliseconds{97});
  unreachable = reservation(Error::no_reachable_datacenter(std::nullopt));
  EXPECT_EQ(unreachable.kind(), Kind::no_reachable_datacenter);
  EXPECT_FALSE(unreachable.best_round_trip());
}

TEST_F(LaunchRefusalSource, AnUnavailableDatacenterIsNamed) {
  const auto unavailable = reservation(
      Error{Stage::select, Reason::datacenter_unavailable}, "missing");
  EXPECT_EQ(unavailable.kind(), Kind::datacenter_unavailable);
  ASSERT_TRUE(unavailable.datacenter());
  EXPECT_EQ(*unavailable.datacenter(), "missing");
}

TEST_F(LaunchRefusalSource, AnAllocationFailureSaysWhyThereIsNoServer) {
  EXPECT_EQ(reservation(Error{Stage::allocate, Reason::network_exhausted})
                .no_server(),
            play::LaunchRefused::NoServer::network);
  EXPECT_EQ(reservation(Error{Stage::allocate, Reason::timed_out}).no_server(),
            play::LaunchRefused::NoServer::queue_timed_out);
  EXPECT_EQ(
      reservation(Error{Stage::allocate, Reason::timed_out_unreachable}).kind(),
      Kind::no_server);
}

TEST_F(LaunchRefusalSource, AnIdentityRefusalMeansTheSignInExpired) {
  EXPECT_EQ(reservation(Error{Stage::identity, Reason::rejected, 403}).kind(),
            Kind::sign_in_expired);
  EXPECT_EQ(reservation(Error{Stage::identity, Reason::invalid_npsso}).kind(),
            Kind::sign_in_expired);
  EXPECT_EQ(reservation(Error{Stage::identity, Reason::transport}).kind(),
            Kind::unavailable);
  EXPECT_EQ(reservation(Error{Stage::identity, Reason::cancelled}).kind(),
            Kind::unavailable);
}

TEST_F(LaunchRefusalSource, AnythingElseIsUnavailable) {
  const auto status =
      reservation(Error{Stage::allocate, Reason::rejected, 503});
  EXPECT_EQ(status.kind(), Kind::unavailable);
  EXPECT_EQ(status.http_status(), 503);
  EXPECT_EQ(play::LaunchFailure::precondition(
                play::LaunchFailure::Precondition::invalid_bitrate)
                .refused()
                .kind(),
            Kind::unavailable);
}

}  // namespace
}  // namespace play_test
