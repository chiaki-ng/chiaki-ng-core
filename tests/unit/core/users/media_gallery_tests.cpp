#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace users_test {
namespace {

namespace users = libchiaking::users;
using Reply = Psn::Reply;

class GalleryCapture : public ::testing::Test {
 protected:
  static constexpr std::string_view gallery_url =
      "https://m.np.playstation.com/api/gameMediaService/v2/c2s/category/"
      "cloudMediaGallery/ugcType/all?ugcIds=psn1&includeTokenizedUrls=true";
  static constexpr std::string_view capture_url =
      "https://m.np.playstation.com/api/gameMediaService/v2/c2s/ugc/psn1/url";

  SignedInRig rig;
};

TEST_F(GalleryCapture, IsFoundUnderStoreAccess) {
  const auto user = rig.signed_in.as(rig.saved(32, "NPSSO-A", "app-a"));
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  rig.fake.on(
      "GET", std::string{gallery_url},
      {Reply{
           200,
           R"({"ugcDocument":[{"id":"psn0","downloadUrl":"https://other"},{"id":"psn1","downloadUrl":"https://file?token=t","fileType":"jpg","fileSize":132722}]})"},
       Reply{
           200,
           R"({"ugcDocument":[{"id":"psn1","screenshotUrl":"https://shot"}]})"},
       Reply{200, R"({"ugcDocument":[]})"}});
  rig.fake.on(
      "GET", std::string{capture_url},
      {Reply{
           200,
           R"({"largePreviewImage":"https://cdn/large.jpg?s=1","screenshotUrl":"https://cdn/shot.jpg?s=2"})"},
       Reply{404, "{}"}, Reply{404, "{}"}});
  users::CatalogCalls calls{*user};
  const auto found = calls.gallery_capture(http, "psn1");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->download_url, "https://cdn/shot.jpg?s=2");
  EXPECT_EQ(found->file_type, "jpg");
  EXPECT_EQ(found->file_size, 132722U);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer app-a");
  EXPECT_EQ(rig.fake.seen.size(), 2U);
  const auto shot = calls.gallery_capture(http, "psn1");
  ASSERT_TRUE(shot);
  EXPECT_EQ(shot->download_url, "https://shot");
  EXPECT_TRUE(shot->file_type.empty());
  const auto missing = calls.gallery_capture(http, "psn1");
  EXPECT_FALSE(missing);
}

class CloudCaptureDownload : public ::testing::Test {
 protected:
  static constexpr std::string_view setting_url =
      "https://m.np.playstation.com/api/settings/v2/users/me/settings?settings="
      "cronosugcautodownloadtomediagallery";

  SignedInRig rig;
};

TEST_F(CloudCaptureDownload, IsReadUnderStoreAccess) {
  const auto user = rig.signed_in.as(rig.saved(33, "NPSSO-A", "app-a"));
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  rig.fake.on(
      "GET", std::string{setting_url},
      {Reply{
           200,
           R"([{"key":"cronosugcautodownloadtomediagallery","value":"on","default":"off"}])"},
       Reply{
           200,
           R"([{"key":"cronosugcautodownloadtomediagallery","default":"off"}])"},
       Reply{200, R"([{"value":"","default":"on"}])"}, Reply{200, R"([])"}});
  users::CatalogCalls calls{*user};
  const auto on = calls.cloud_capture_download(http);
  ASSERT_TRUE(on);
  EXPECT_TRUE(on.value());
  const auto off = calls.cloud_capture_download(http);
  ASSERT_TRUE(off);
  EXPECT_FALSE(off.value());
  const auto defaulted = calls.cloud_capture_download(http);
  ASSERT_TRUE(defaulted);
  EXPECT_TRUE(defaulted.value());
  EXPECT_FALSE(calls.cloud_capture_download(http));
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer app-a");
}

}  // namespace
}  // namespace users_test
