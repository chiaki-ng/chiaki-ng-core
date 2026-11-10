#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

namespace catalog_api_test {
namespace {

class CatalogCloudLeftOut : public CatalogApiTest {};

TEST_F(CatalogCloudLeftOut, RefusesEveryCloudCallWithoutReachingPs) {
  Rig left_out;
  const auto user = left_out.user();
  const auto title = product(rivet);
  const auto list = left_out.catalog.get_streamable_list(*user);
  ASSERT_FALSE(list);
  EXPECT_EQ(list.error().reason(), catalog::StreamableListReason::not_built);
  const auto games = left_out.catalog.process_library_and_list(*user);
  ASSERT_FALSE(games);
  EXPECT_EQ(games.error().reason(), catalog::GamesReason::not_built);
  const auto access = left_out.catalog.cloud_access_of(*user);
  ASSERT_FALSE(access);
  EXPECT_EQ(access.error().reason(), catalog::CloudAccessReason::not_built);
  const auto store =
      left_out.catalog.check_title_against_storefront(*user, title);
  ASSERT_FALSE(store);
  EXPECT_EQ(store.error().reason(), catalog::StorefrontReason::not_built);
  const auto owned = left_out.catalog.recheck_ownership(*user, title);
  ASSERT_FALSE(owned);
  EXPECT_EQ(owned.error().reason(), catalog::OwnershipRecheckReason::not_built);
  const auto capture = left_out.catalog.cloud_capture(*user, "gallery");
  ASSERT_FALSE(capture);
  EXPECT_EQ(capture.error().reason(), catalog::CloudCaptureReason::not_built);
  const auto download = left_out.catalog.cloud_capture_download(*user);
  ASSERT_FALSE(download);
  EXPECT_EQ(download.error().reason(),
            catalog::CaptureDownloadReason::not_built);
  const auto cleared = left_out.catalog.clear_for_launch(*user, title);
  ASSERT_FALSE(cleared.is_allowed());
  ASSERT_NE(cleared.refusal(), nullptr);
  ASSERT_TRUE(cleared.refusal()->cause());
  EXPECT_EQ(cleared.refusal()->cause()->reason(),
            catalog::LaunchCheckReason::not_built);
  EXPECT_FALSE(left_out.catalog.can_launch(*user, title).is_allowed());
  EXPECT_TRUE(left_out.http.seen().empty());
}

}  // namespace
}  // namespace catalog_api_test
