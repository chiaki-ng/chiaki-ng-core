#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace catalog_api_test {
namespace {

using SharedRequests = CatalogApiTest;

TEST_F(SharedRequests, AnswerEveryoneAskingForTheListAndLetALeaverGo) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.hold(std::string{old_service_stores});

  libchiaking::Expected<std::shared_ptr<const catalog::StreamableList>,
                        catalog::StreamableListFailure>
      led{libchiaking::unexpected(
          unanswered<catalog::StreamableListFailure>())};
  auto waited = led;
  auto gave_up = led;
  libchiaking::platform::StopSource impatient;
  auto leader = started([&] { led = rig.catalog.get_streamable_list(*user); });
  rig.http.wait_until_held();
  auto waiter =
      started([&] { waited = rig.catalog.get_streamable_list(*user); });
  auto quitter = started([&] {
    gave_up = rig.catalog.get_streamable_list(*user, impatient.token());
  });
  pause_a_moment();
  impatient.request_stop();
  EXPECT_TRUE(quitter.join());
  ASSERT_FALSE(gave_up);
  EXPECT_TRUE(gave_up.error().is_cancelled());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);

  rig.http.release();
  EXPECT_TRUE(leader.join());
  EXPECT_TRUE(waiter.join());
  ASSERT_TRUE(led);
  ASSERT_TRUE(waited);
  EXPECT_EQ(led.value(), waited.value());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(all_ps5_list), 1);
  const auto after = rig.catalog.get_streamable_list(*user);
  ASSERT_TRUE(after);
  EXPECT_EQ(after.value(), led.value());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
}

TEST_F(SharedRequests, AnswerEveryoneAskingTheStorefront) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  storefront_says(rig, rivet, streams_notice);
  rig.http.hold(std::string{store_app});
  libchiaking::Expected<StorefrontAnswer, catalog::StorefrontFailure> told{
      libchiaking::unexpected(unanswered<catalog::StorefrontFailure>())};
  auto told_again = told;
  auto checking = started([&] {
    told = rig.catalog.check_title_against_storefront(*user, product(rivet));
  });
  rig.http.wait_until_held();
  auto also_checking = started([&] {
    told_again =
        rig.catalog.check_title_against_storefront(*user, product(rivet));
  });
  pause_a_moment();
  rig.http.release();
  EXPECT_TRUE(checking.join());
  EXPECT_TRUE(also_checking.join());
  ASSERT_TRUE(told);
  ASSERT_TRUE(told_again);
  EXPECT_EQ(told.value(), told_again.value());
  EXPECT_TRUE(told.value().says_it_streams());
  EXPECT_EQ(rig.http.count(store_app), 1);
}

TEST_F(SharedRequests, AnswerEveryoneAskingForTheLibrary) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  rig.http.hold(std::string{owned_list});
  libchiaking::Expected<std::shared_ptr<const catalog::Library>,
                        catalog::LibraryFailure>
      first{libchiaking::unexpected(unanswered<catalog::LibraryFailure>())};
  auto second = first;
  auto asking = started([&] { first = rig.catalog.library_of(*user); });
  rig.http.wait_until_held();
  auto also_asking = started([&] { second = rig.catalog.library_of(*user); });
  pause_a_moment();
  rig.http.release();
  EXPECT_TRUE(asking.join());
  EXPECT_TRUE(also_asking.join());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value(), second.value());
  EXPECT_EQ(rig.http.count(owned_list), 1);
}

TEST_F(SharedRequests,
       RefuseTwoLaunchesOfAnIncludedTitleAlikeWithoutTheStorefront) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  cloud_streaming_allowed(rig);
  storefront_says(rig, comet, streams_notice + comet_library_offer);
  (void)loaded(rig, *user);
  auto first = rig.catalog.can_launch(*user, product(comet));
  auto second = first;
  auto launching = started(
      [&] { first = rig.catalog.clear_for_launch(*user, product(comet)); });
  auto also_launching = started(
      [&] { second = rig.catalog.clear_for_launch(*user, product(comet)); });
  EXPECT_TRUE(launching.join());
  EXPECT_TRUE(also_launching.join());
  ASSERT_EQ(refusal_kind(first), LaunchRefusal::Kind::add_to_library);
  ASSERT_EQ(refusal_kind(second), LaunchRefusal::Kind::add_to_library);
  EXPECT_TRUE(first.refusal()->library_offer());
  EXPECT_EQ(first.refusal()->library_offer(),
            second.refusal()->library_offer());
  EXPECT_EQ(rig.http.count(store_app), 0);
}

}  // namespace
}  // namespace catalog_api_test
