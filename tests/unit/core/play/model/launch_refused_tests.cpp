#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
using libchiaking::play::DatacenterName;
using libchiaking::play::LaunchRefused;
using Kind = LaunchRefused::Kind;
using std::chrono::milliseconds;

TEST(LaunchRefused, CarriesTheCatalogClearance) {
  const auto not_cleared = LaunchRefused::not_cleared(
      catalog::LaunchRefusal::needs_premium(libchiaking::users::Tier::extra));
  EXPECT_EQ(not_cleared.kind(), Kind::not_cleared);
  ASSERT_TRUE(not_cleared.clearance());
  EXPECT_EQ(not_cleared.clearance()->kind(),
            catalog::LaunchRefusal::Kind::needs_premium);
  EXPECT_EQ(not_cleared.clearance()->tier_held(),
            libchiaking::users::Tier::extra);
  EXPECT_FALSE(not_cleared.is_reported_to_catalog());
  EXPECT_FALSE(not_cleared.ps_refusal());
}

TEST(LaunchRefused, CarriesThePrivacyUpgradeUrl) {
  const auto privacy =
      LaunchRefused::privacy_settings("https://example.test/upgrade?a=1");
  ASSERT_TRUE(privacy);
  EXPECT_EQ(privacy->kind(), Kind::privacy_settings);
  EXPECT_EQ(privacy->upgrade_url(), "https://example.test/upgrade?a=1");
  EXPECT_FALSE(privacy->clearance());
  const auto empty = LaunchRefused::privacy_settings("");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->upgrade_url().empty());
}

TEST(LaunchRefused, RefusesAMalformedPrivacyUrl) {
  EXPECT_FALSE(LaunchRefused::privacy_settings("https://example.test/a b"));
  EXPECT_FALSE(LaunchRefused::privacy_settings("https://example.test/\n"));
  EXPECT_TRUE(LaunchRefused::privacy_settings(
      std::string(LaunchRefused::url_limit, 'a')));
  EXPECT_FALSE(LaunchRefused::privacy_settings(
      std::string(LaunchRefused::url_limit + 1, 'a')));
}

TEST(LaunchRefused, CarriesTheDisplayPriceOfALicence) {
  const auto priced = LaunchRefused::licence_not_free(
      "\xe2\x82\xac"
      "9.99");
  ASSERT_TRUE(priced);
  EXPECT_EQ(priced->kind(), Kind::licence_not_free);
  EXPECT_EQ(priced->display_price(),
            "\xe2\x82\xac"
            "9.99");
  EXPECT_TRUE(LaunchRefused::licence_not_free(""));
  EXPECT_FALSE(LaunchRefused::licence_not_free("9\t99"));
  EXPECT_FALSE(LaunchRefused::licence_not_free(
      std::string(LaunchRefused::price_limit + 1, '9')));
}

TEST(LaunchRefused, NamesTheRefusalsWithoutPayload) {
  EXPECT_EQ(LaunchRefused::membership_rejected().kind(),
            Kind::membership_rejected);
  EXPECT_EQ(LaunchRefused::outside_streaming_plan().kind(),
            Kind::outside_streaming_plan);
  EXPECT_EQ(LaunchRefused::store_region().kind(), Kind::store_region);
  EXPECT_EQ(LaunchRefused::title_not_found().kind(), Kind::title_not_found);
  EXPECT_EQ(LaunchRefused::sign_in_expired().kind(), Kind::sign_in_expired);
}

TEST(LaunchRefused, ReportsAServiceRefusalToTheCatalog) {
  const auto ps = LaunchRefused::refused_by_service(
      catalog::StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_EQ(ps.kind(), Kind::refused_by_service);
  EXPECT_TRUE(ps.is_reported_to_catalog());
  EXPECT_EQ(ps.ps_refusal(),
            catalog::StreamRefusal::Kind::no_game_for_entitlement);
  EXPECT_EQ(LaunchRefused::refused_by_service(
                catalog::StreamRefusal::Kind::refused_by_stream_server)
                .ps_refusal(),
            catalog::StreamRefusal::Kind::refused_by_stream_server);
}

TEST(LaunchRefused, CarriesTheBestRoundTripWhenNoDatacenterIsReachable) {
  const auto far = LaunchRefused::no_reachable_datacenter(milliseconds{140});
  EXPECT_EQ(far.kind(), Kind::no_reachable_datacenter);
  EXPECT_EQ(far.best_round_trip(), milliseconds{140});
  EXPECT_TRUE(far.is_about_datacenters());
  EXPECT_FALSE(far.is_reported_to_catalog());
  EXPECT_FALSE(
      LaunchRefused::no_reachable_datacenter(std::nullopt).best_round_trip());
  EXPECT_FALSE(LaunchRefused::no_reachable_datacenter(milliseconds{0})
                   .best_round_trip());
}

TEST(LaunchRefused, NamesAnUnavailableDatacenter) {
  const auto gone =
      LaunchRefused::datacenter_unavailable(*DatacenterName::parse("usw2"));
  EXPECT_EQ(gone.kind(), Kind::datacenter_unavailable);
  ASSERT_TRUE(gone.datacenter());
  EXPECT_EQ(*gone.datacenter(), "usw2");
  EXPECT_TRUE(gone.is_about_datacenters());
}

TEST(LaunchRefused, SaysWhyNoServerWasFound) {
  const auto queue =
      LaunchRefused::no_server(LaunchRefused::NoServer::queue_timed_out);
  EXPECT_EQ(queue.kind(), Kind::no_server);
  EXPECT_EQ(queue.no_server(), LaunchRefused::NoServer::queue_timed_out);
  EXPECT_EQ(
      LaunchRefused::no_server(LaunchRefused::NoServer::network).no_server(),
      LaunchRefused::NoServer::network);
  EXPECT_FALSE(queue.is_about_datacenters());
  EXPECT_FALSE(queue.datacenter());
  EXPECT_FALSE(queue.best_round_trip());
}

TEST(LaunchRefused, CarriesTheHttpStatusOfAnUnavailableService) {
  const auto unavailable = LaunchRefused::unavailable(503);
  EXPECT_EQ(unavailable.kind(), Kind::unavailable);
  EXPECT_EQ(unavailable.http_status(), 503);
  EXPECT_EQ(LaunchRefused::unavailable().http_status(), 0);
  EXPECT_FALSE(unavailable.no_server());
}

TEST(LaunchRefused, ReportsOnlyServiceRefusalsToTheCatalog) {
  const auto not_cleared = LaunchRefused::not_cleared(
      catalog::LaunchRefusal::needs_premium(libchiaking::users::Tier::extra));
  const auto privacy =
      LaunchRefused::privacy_settings("https://example.test/upgrade?a=1");
  const auto priced = LaunchRefused::licence_not_free(
      "\xe2\x82\xac"
      "9.99");
  ASSERT_TRUE(privacy);
  ASSERT_TRUE(priced);
  for (const auto& other :
       {not_cleared, *privacy, *priced, LaunchRefused::membership_rejected(),
        LaunchRefused::outside_streaming_plan(), LaunchRefused::store_region(),
        LaunchRefused::title_not_found(),
        LaunchRefused::no_reachable_datacenter(milliseconds{140}),
        LaunchRefused::datacenter_unavailable(*DatacenterName::parse("usw2")),
        LaunchRefused::no_server(LaunchRefused::NoServer::queue_timed_out),
        LaunchRefused::sign_in_expired(), LaunchRefused::unavailable(503)}) {
    SCOPED_TRACE(testing::PrintToString(other.kind()));
    EXPECT_FALSE(other.is_reported_to_catalog());
  }
}

}  // namespace
}  // namespace play_test
