#include "src/core/psn/rest/store/store_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::StoreClient;
using libchiaking::psn::rest::StoreError;
using Reply = RestRig::Reply;
using Lists = std::vector<glz::raw_json>;

class StoreProductPage : public ::testing::Test {
 protected:
  static constexpr std::string_view product =
      "https://store.playstation.com/en-gb/product/"
      "EP0000-CUSA12345_00-TITLE0000000000";

  RestRig rig;
  StoreClient client{rig.http};
};

TEST_F(StoreProductPage, IsCheckedWithAHeadRequest) {
  rig.fake.on("HEAD", std::string{product}, {Reply{200, ""}});
  const auto exists = client.product_page_exists(product);
  ASSERT_TRUE(exists);
  EXPECT_TRUE(exists.value());
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.method, "HEAD");
  EXPECT_TRUE(seen.headers.empty());
  EXPECT_EQ(seen.total_timeout, std::chrono::milliseconds{5000});
  EXPECT_EQ(seen.connect_timeout, std::chrono::milliseconds{3000});
}

TEST_F(StoreProductPage, IsMissingWhenTheStoreAnswers404) {
  rig.fake.on("HEAD", std::string{product}, {Reply{404, ""}});
  const auto missing = client.product_page_exists(product);
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing.value());
}

TEST_F(StoreProductPage, ReportsATransportFailure) {
  rig.fake.on("HEAD", std::string{product}, {Reply::transport()});
  const auto failed = client.product_page_exists(product);
  ASSERT_FALSE(failed);
  EXPECT_TRUE(failed.error().failure());
}

class StoreGamesLists : public ::testing::Test {
 protected:
  static constexpr std::string_view base =
      "https://www.playstation.com/bin/imagic/"
      "gameslist?locale=en-gb&categoryList=";

  RestRig rig;
  StoreClient client{rig.http};
};

TEST_F(StoreGamesLists, AreRequestedByLocale) {
  rig.fake.on("GET", std::string{base} + "plus-games-list", {Reply{200, "[]"}});
  const auto lists = client.games_lists<Lists>("en-gb", "plus-games-list");
  ASSERT_TRUE(lists);
  EXPECT_TRUE(lists.value().empty());
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  ASSERT_EQ(seen.headers.size(), 3U);
  EXPECT_EQ(seen.headers[0].name, "Content-Type");
  EXPECT_EQ(seen.headers[0].value, "application/json");
  EXPECT_EQ(seen.headers[1].name, "Accept");
  EXPECT_EQ(seen.headers[1].value, "application/json");
  EXPECT_EQ(seen.headers[2].name, "User-Agent");
  EXPECT_EQ(seen.headers[2].value,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
  EXPECT_EQ(seen.total_timeout, libchiaking::net::http::default_total_timeout);
}

TEST_F(StoreGamesLists, RejectARefusal) {
  rig.fake.on("GET", std::string{base} + "a", {Reply{404, "[]"}});
  const auto missing = client.games_lists<Lists>("en-gb", "a");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(missing.error().http_status(), 404);
}

TEST_F(StoreGamesLists, RejectAnObjectReply) {
  rig.fake.on("GET", std::string{base} + "b", {Reply{200, "{}"}});
  const auto object = client.games_lists<Lists>("en-gb", "b");
  ASSERT_FALSE(object);
  EXPECT_EQ(object.error().reason(), StoreError::Reason::malformed);
}

}  // namespace
}  // namespace psn_test
