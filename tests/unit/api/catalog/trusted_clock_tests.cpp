#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace catalog_api_test {
namespace {

class TrustedTime : public CatalogApiTest {
 protected:
  static auto release() {
    return catalog::ReleaseDate::parse_time("2031-01-01T00:00:00.000Z");
  }

  static void dated_rivet_is_owned(Rig& rig) {
    old_service_lists(rig);
    ps5_store_lists(rig, store_list({rivet_listed}));
    library_holds(rig, owned_page({dated_rivet_owned}));
    membership_is(rig, "TIER_30");
    cloud_streaming_allowed(rig);
    storefront_says(rig, rivet, streams_notice);
  }
};

TEST_F(TrustedTime, JudgesAReleaseDateLastAndAlone) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  const auto released = release();
  ASSERT_TRUE(released);
  EXPECT_GT(*released, rig.now());
  dated_rivet_is_owned(rig);
  (void)loaded(rig, *user);
  EXPECT_GT(rig.server.refreshes.load(), 0);

  EXPECT_FALSE(rig.catalog.trusted_time());
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::no_trusted_clock);
  const auto unsure = rig.catalog.clear_for_launch(*user, product(rivet));
  EXPECT_EQ(refusal_kind(unsure), LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(rig.http.count(cloud_benefits), 2);
  EXPECT_EQ(rig.http.count(subscriptions), 0);
  EXPECT_EQ(rig.http.count(store_app), 0);

  rig.server.current = *released - std::chrono::milliseconds{1};
  const auto refreshes = rig.server.refreshes.load();
  EXPECT_EQ(rig.catalog.trusted_time(), rig.server.current);
  EXPECT_EQ(rig.server.refreshes.load(), refreshes);
  const auto early = rig.catalog.clear_for_launch(*user, product(rivet));
  ASSERT_EQ(refusal_kind(early), LaunchRefusal::Kind::not_released);
  EXPECT_EQ(early.refusal()->time(), *released);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_released);

  rig.server.current = *released;
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_released);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::not_released);

  rig.server.current = *released + std::chrono::milliseconds{1};
  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).is_allowed());
}

TEST_F(TrustedTime, ShowsEachTimeServerTheLoadAsks) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  dated_rivet_is_owned(rig);
  rig.server.asking_hosts = {"time.cloudflare.com", "time1.google.com",
                             "time1.google.com", "time2.google.com"};
  RecordedFetch recorded;
  (void)rig.catalog.process_library_and_list(*user, {}, recorded.observer());
  std::vector<std::string> shown;
  for (const auto& state : recorded.states()) {
    if (state.step() != FetchStep::trusted_time) {
      EXPECT_FALSE(state.time_server());
      continue;
    }
    if (const auto& server = state.time_server()) {
      EXPECT_EQ(state.activity(), FetchActivity::syncing_clock);
      shown.push_back(*server);
    }
  }
  EXPECT_EQ(shown,
            (std::vector<std::string>{"time.cloudflare.com", "time1.google.com",
                                      "time2.google.com"}));
  EXPECT_FALSE(recorded.states().back().time_server());
}

TEST_F(TrustedTime, IsSyncedByALaunchAndThePreCheckThenAgrees) {
  Rig rig;
  rig.hold_exclusions();
  const auto user = rig.user();
  const auto released = release();
  ASSERT_TRUE(released);
  dated_rivet_is_owned(rig);
  (void)loaded(rig, *user);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::no_trusted_clock);

  rig.server.synced = *released + std::chrono::hours{1};
  EXPECT_TRUE(rig.catalog.clear_for_launch(*user, product(rivet)).is_allowed());
  EXPECT_TRUE(rig.catalog.can_launch(*user, product(rivet)).is_allowed());
}

TEST_F(TrustedTime, IsNeverAskedForAnUndatedListing) {
  Rig rig;
  const auto user = rig.user();
  ps_as_usual(rig);
  (void)loaded(rig, *user);
  EXPECT_EQ(rig.server.refreshes.load(), 0);
}

TEST_F(TrustedTime, IsNeverAskedOfPsByALoadThatStillOffersADatedTitle) {
  Rig rig;
  libchiaking::testing::InMemoryRecordStore records{shared_threading};
  libchiaking::users::RequestBudget budget{shared_threading, rig.clock};
  Catalog clockless{libchiaking::testing::ServiceBuilder::build<Catalog>(
      rig.http, records, shared_threading, rig.clock, rig.wall, budget)};
  const auto user = rig.user();
  const auto released = release();
  ASSERT_TRUE(released);
  EXPECT_GT(*released, rig.now());
  dated_rivet_is_owned(rig);

  EXPECT_TRUE(clockless.get_streamable_list(*user));
  const auto games = clockless.process_library_and_list(*user);
  ASSERT_TRUE(games);
  EXPECT_EQ(rig.http.count(device_check), 0);
  for (const auto& asked : rig.http.seen()) {
    EXPECT_NE(asked.method, "HEAD");
    EXPECT_NE(asked.url, "https://www.playstation.com/");
  }
  EXPECT_EQ(game_of(*games.value(), rivet).release().time(), *released);
  EXPECT_EQ(rig.http.count(Rig::feature_config), 1);
  EXPECT_EQ(refusal_kind(clockless.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::no_trusted_clock);
  const auto unsure = clockless.clear_for_launch(*user, product(rivet));
  EXPECT_EQ(rig.http.count(Rig::feature_config), 1);
  EXPECT_EQ(refusal_kind(unsure), LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(refusal_kind(clockless.can_launch(*user, product(rivet))),
            LaunchRefusal::Kind::no_trusted_clock);
  EXPECT_EQ(rig.http.count(device_check), 0);
  rig.server.current = *released;
  EXPECT_FALSE(clockless.trusted_time());
}

}  // namespace
}  // namespace catalog_api_test
