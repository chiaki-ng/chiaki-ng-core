#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/cloud/cloud_launch/provision_rig.hpp"

#include <libchiaking/testing/in_memory_credential_store.hpp>
#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <variant>

namespace cloud_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace users = libchiaking::users;
using NotCleared = play::LaunchFailure::NotCleared;
using Refused = play::LaunchRefused::Kind;

class CloudLaunchChecks : public ::testing::Test, protected ProvisionRig {
 protected:
  void SetUp() override {
    launched.title.product = "EP0109-PPSA07033_00-FAKE0030TITLE000";
  }

  void sign_in() {
    EXPECT_TRUE(signed_in.sign_out(libchiaking::AccountId{1}));
    EXPECT_TRUE(credentials.save(test_support::play::SignedInPlayer::saved(
        "synthetic-token", "000000070041008042424242424242424242424242424242",
        libchiaking::AccountId{1})));
    EXPECT_TRUE(signed_in.welcome_back());
    launched.user = signed_in.find(libchiaking::AccountId{1});
  }

  void launch_legacy_title() {
    launched.title = test_support::play::Title{
        .product = "UP0114-CUSA00001_00-FAKE0102TITLE000",
        .service = catalog::ServiceGeneration::old_service,
        .platform = catalog::Platform::ps4,
        .store = "en-GB"};
    launched.skip_account_attributes_check = true;
  }

  test_support::FakeClock monotonic{5'000'000};
  test_support::play::LaunchCase launched;
  test_support::users::Offline offline;
  libchiaking::testing::InMemoryCredentialStore credentials{
      test_support::process_threading()};
  test_support::users::SystemWall user_wall;
  libchiaking::platform::SystemSecureRandom user_random;
  users::SignedInUsers signed_in{
      libchiaking::testing::ServiceBuilder::build<users::SignedInUsers>(
          offline, credentials, test_support::process_threading(), monotonic,
          user_wall, user_random)};
  GatewayProvider gk_provider;
};

TEST_F(CloudLaunchChecks, RefusesAMissingUserBeforeAnyRequest) {
  gk_provider.preflight_expected = true;
  CheckedProvider checked{gk_provider};
  auto clearance = launched.clearance();
  const auto refused = provision_cleared(launched, checked, clearance);
  ASSERT_FALSE(refused);
  EXPECT_EQ(gk_provider.preflight_calls, 0U);
  EXPECT_EQ(gk_provider.calls, 0U);
  EXPECT_TRUE(checked.graphql.empty());
  EXPECT_EQ(clearance.clears, 0);
  EXPECT_TRUE(clearance.remembered.empty());
  const auto* error =
      std::get_if<play::LaunchFailure::Unmet>(&refused.error().cause());
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->precondition(),
            play::LaunchFailure::Precondition::invalid_npsso);
  EXPECT_TRUE(play::LinkFailure::unlaunched(refused.error()).message().empty());
  EXPECT_TRUE(refused.error().detail().empty());
}

TEST_F(CloudLaunchChecks, StopsWhenTheCatalogCannotAnswer) {
  sign_in();
  gk_provider.preflight_expected = true;
  CheckedProvider checked{gk_provider};
  launched.refusal =
      catalog::LaunchRefusal::check_unavailable(catalog::LaunchCheckFailure{
          catalog::LaunchCheckReason::user, 0,
          users::Unanswered{users::Unanswered::Reason::store_access_lapsed}});
  auto clearance = launched.clearance();
  const auto refused = provision_cleared(launched, checked, clearance);
  ASSERT_FALSE(refused);
  EXPECT_EQ(gk_provider.calls, 0U);
  EXPECT_TRUE(checked.graphql.empty());
  EXPECT_EQ(clearance.clears, 1);
  EXPECT_TRUE(clearance.remembered.empty());
  const auto* error = std::get_if<NotCleared>(&refused.error().cause());
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->unanswered(),
            libchiaking::psn::graphql::CheckStatus::no_app_token);
  EXPECT_EQ(refused.error().refused().kind(), Refused::not_cleared);
}

TEST_F(CloudLaunchChecks, AsksToAddACatalogTitleToTheLibraryFirst) {
  sign_in();
  gk_provider.preflight_expected = true;
  CheckedProvider checked{gk_provider};
  launched.refusal = catalog::LaunchRefusal::add_to_library(
      catalog::StorefrontAnswer::LibraryOffer{
          "EP0109-PPSA07033_00-FAKE0030TITLE000-E002",
          {"EP0109-PPSA07033_00-FAKE0030TITLE000"},
          false});
  auto clearance = launched.clearance();
  const auto refused = provision_cleared(launched, checked, clearance);
  ASSERT_FALSE(refused);
  EXPECT_EQ(gk_provider.calls, 0U);
  EXPECT_EQ(checked.others.size(), std::size_t{1});
  EXPECT_EQ(clearance.clears, 1);
  EXPECT_TRUE(clearance.remembered.empty());
  const auto* error = std::get_if<NotCleared>(&refused.error().cause());
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->refusal().kind(),
            catalog::LaunchRefusal::Kind::add_to_library);
  ASSERT_TRUE(error->refusal().library_offer());
  EXPECT_EQ(error->refusal().library_offer()->sku_id,
            "EP0109-PPSA07033_00-FAKE0030TITLE000-E002");
  const auto detail = play::LinkFailure::unlaunched(refused.error());
  EXPECT_EQ(detail.stage(), libchiaking::net::protocol::remote_play::
                                FailureStage::cloud_launch_check);
  EXPECT_EQ(
      detail.describe(),
      "the PlayStation Plus catalog title has not been added to the library");
  const auto typed = refused.error().refused();
  EXPECT_EQ(typed.kind(), Refused::not_cleared);
  EXPECT_FALSE(typed.is_reported_to_catalog());
  ASSERT_TRUE(typed.clearance());
  EXPECT_EQ(typed.clearance()->kind(),
            catalog::LaunchRefusal::Kind::add_to_library);
}

TEST_F(CloudLaunchChecks, ProvisionsAClearedTitle) {
  sign_in();
  gk_provider.preflight_expected = true;
  CheckedProvider checked{gk_provider};
  auto clearance = launched.clearance();
  EXPECT_TRUE(provision_cleared(launched, checked, clearance));
  EXPECT_EQ(gk_provider.calls, 10U);
  EXPECT_TRUE(checked.graphql.empty());
  EXPECT_EQ(checked.others.size(), std::size_t{11});
  EXPECT_EQ(clearance.clears, 1);
  EXPECT_TRUE(clearance.remembered.empty());
  ASSERT_FALSE(clearance.cleared_products.empty());
  EXPECT_EQ(clearance.cleared_products.front(),
            "EP0109-PPSA07033_00-FAKE0030TITLE000");
}

TEST_F(CloudLaunchChecks, ResolvesAClearedLegacyTitle) {
  sign_in();
  launch_legacy_title();
  RetryProvider legacy_cloud_stream;
  CheckedProvider checked{legacy_cloud_stream};
  auto clearance = launched.clearance();
  EXPECT_TRUE(provision_cleared(launched, checked, clearance));
  EXPECT_EQ(legacy_cloud_stream.lookups, 1U);
  EXPECT_EQ(legacy_cloud_stream.starts, 1U);
  EXPECT_TRUE(checked.graphql.empty());
  EXPECT_EQ(clearance.clears, 1);
}

TEST_F(CloudLaunchChecks, StopsALegacyTitleThatNeedsPremium) {
  sign_in();
  launch_legacy_title();
  RetryProvider legacy_cloud_stream;
  CheckedProvider checked{legacy_cloud_stream};
  launched.refusal = catalog::LaunchRefusal::needs_premium(users::Tier::none);
  auto clearance = launched.clearance();
  const auto refused = provision_cleared(launched, checked, clearance);
  ASSERT_FALSE(refused);
  EXPECT_EQ(legacy_cloud_stream.lookups, 0U);
  EXPECT_EQ(legacy_cloud_stream.starts, 0U);
  EXPECT_EQ(legacy_cloud_stream.account_sessions, 0U);
  const auto* error = std::get_if<NotCleared>(&refused.error().cause());
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->refusal().kind(),
            catalog::LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(error->refusal().tier_held(), users::Tier::none);
  EXPECT_TRUE(clearance.remembered.empty());
}

}  // namespace
}  // namespace cloud_test
