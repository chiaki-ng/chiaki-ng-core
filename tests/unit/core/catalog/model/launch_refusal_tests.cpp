#include "src/core/catalog/model/unanswered.hpp"
#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace catalog_test {
namespace {

using libchiaking::catalog::GamesReason;
using libchiaking::catalog::HideReason;
using libchiaking::catalog::LaunchCheckReason;
using libchiaking::catalog::LaunchRefusal;
using libchiaking::catalog::LaunchVerdict;
using libchiaking::catalog::LibraryReason;
using libchiaking::catalog::Unanswered;
using libchiaking::users::Tier;
using Kind = LaunchRefusal::Kind;

class TypedLaunchRefusals : public ValueCases {};

TEST_F(TypedLaunchRefusals, AreAbsentFromAnAllowedVerdict) {
  EXPECT_TRUE(LaunchVerdict::allowed().is_allowed());
  EXPECT_FALSE(LaunchVerdict::allowed().refusal());
}

TEST_F(TypedLaunchRefusals, CarryTheReasonATitleIsHidden) {
  const auto hidden = LaunchVerdict::refused(
      LaunchRefusal::hidden(HideReason::tier_does_not_include));
  EXPECT_FALSE(hidden.is_allowed());
  ASSERT_TRUE(hidden.refusal());
  EXPECT_EQ(hidden.refusal()->kind(), Kind::hidden);
  EXPECT_EQ(hidden.refusal()->hide_reason(), HideReason::tier_does_not_include);
}

TEST_F(TypedLaunchRefusals, CarryTheirDetails) {
  const libchiaking::catalog::StorefrontAnswer::LibraryOffer offer{
      "SKU-1", {"ENT-1"}, false};
  EXPECT_EQ(LaunchRefusal::add_to_library(offer).library_offer(), offer);
  EXPECT_FALSE(LaunchRefusal::add_to_library().library_offer());
  EXPECT_EQ(LaunchRefusal::needs_premium(Tier::extra).tier_held(), Tier::extra);
  EXPECT_FALSE(LaunchRefusal::needs_premium(std::nullopt).tier_held());
  EXPECT_EQ(LaunchRefusal::refused_by_service(at_ms(5000)).time(), at_ms(5000));
  EXPECT_EQ(LaunchRefusal::not_streamable("notice").notice(), "notice");
  EXPECT_EQ(LaunchRefusal::not_released(at_ms(7000)).time(), at_ms(7000));
}

TEST_F(TypedLaunchRefusals, NameTheirKind) {
  EXPECT_EQ(LaunchRefusal::add_to_library().kind(), Kind::add_to_library);
  EXPECT_EQ(LaunchRefusal::not_listed().kind(), Kind::not_listed);
  EXPECT_EQ(LaunchRefusal::buy_to_stream().kind(), Kind::buy_to_stream);
  EXPECT_EQ(LaunchRefusal::unannounced().kind(), Kind::unannounced);
  EXPECT_EQ(LaunchRefusal::no_trusted_clock().kind(), Kind::no_trusted_clock);
  EXPECT_EQ(LaunchRefusal::invalid_title().kind(), Kind::invalid_title);
  EXPECT_EQ(LaunchRefusal::title_taken_down().kind(), Kind::title_taken_down);
  EXPECT_EQ(LaunchRefusal::streaming_unavailable().kind(),
            Kind::streaming_unavailable);
  EXPECT_EQ(LaunchRefusal::purchase_needed().kind(), Kind::purchase_needed);
  EXPECT_EQ(LaunchRefusal::unexpected_entitlement_state().kind(),
            Kind::unexpected_entitlement_state);
  EXPECT_EQ(LaunchRefusal::choose_a_game().kind(), Kind::choose_a_game);
}

TEST_F(TypedLaunchRefusals, CarryNothingTheyDoNotNeed) {
  EXPECT_FALSE(LaunchRefusal::title_taken_down().time());
  EXPECT_TRUE(LaunchRefusal::purchase_needed().notice().empty());
  EXPECT_FALSE(LaunchRefusal::invalid_title().cause());
}

TEST_F(TypedLaunchRefusals, CarryTheCauseOfAnUnavailableCheck) {
  const libchiaking::users::Unanswered expired{
      libchiaking::users::Unanswered::Reason::sign_in_expired, 401};
  const auto unavailable = LaunchRefusal::check_unavailable(
      Unanswered{expired}.as(LaunchCheckReason::unavailable));
  EXPECT_EQ(unavailable.kind(), Kind::check_unavailable);
  ASSERT_TRUE(unavailable.cause());
  EXPECT_TRUE(unavailable.cause()->needs_sign_in());
  EXPECT_EQ(unavailable.cause()->reason(), LaunchCheckReason::user);
  EXPECT_EQ(unavailable.cause()->http_status(), 401);
  EXPECT_FALSE(unavailable.cause()->is_cancelled());
}

TEST(CatalogUnanswered, IsCancelledWhenTheUserCallWas) {
  const libchiaking::users::Unanswered stopped{
      libchiaking::users::Unanswered::Reason::cancelled, 0};
  EXPECT_TRUE(Unanswered{stopped}.is_cancelled());
  EXPECT_TRUE(Unanswered::cancelled().is_cancelled());
}

TEST(CatalogUnanswered, HasNoUserCauseOfItsOwn) {
  const Unanswered region{Unanswered::Reason::region_unsupported, 404};
  EXPECT_FALSE(region.cause());
  EXPECT_FALSE(region.needs_sign_in());
  EXPECT_EQ(region.http_status(), 404);
}

TEST(CatalogUnanswered, NarrowsToTheCallsReasonOfTheSameName) {
  const Unanswered region{Unanswered::Reason::region_unsupported, 404};
  const auto narrowed = region.as(GamesReason::unavailable);
  EXPECT_EQ(narrowed.reason(), GamesReason::region_unsupported);
  EXPECT_EQ(narrowed.http_status(), 404);
  EXPECT_FALSE(narrowed.cause());
}

TEST(CatalogUnanswered, NarrowsAReasonTheCallLacksToTheGivenOne) {
  const Unanswered storage{Unanswered::Reason::storage};
  EXPECT_EQ(storage.as(LibraryReason::malformed).reason(),
            LibraryReason::malformed);
}

TEST(CatalogUnanswered, KeepsTheUserCauseWhenNarrowed) {
  const libchiaking::users::Unanswered expired{
      libchiaking::users::Unanswered::Reason::sign_in_expired, 401};
  const auto narrowed = Unanswered{expired}.as(LibraryReason::malformed);
  EXPECT_EQ(narrowed.reason(), LibraryReason::user);
  EXPECT_TRUE(narrowed.needs_sign_in());
  EXPECT_EQ(narrowed.http_status(), 401);
}

}  // namespace
}  // namespace catalog_test
