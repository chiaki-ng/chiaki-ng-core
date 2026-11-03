#include "tests/unit/core/psn/rest/kamaji/kamaji_rig.hpp"

#include <gtest/gtest.h>
#include <glaze/json.hpp>

#include <string>

namespace psn_test {
namespace {

using libchiaking::psn::rest::kamaji_locale_component_capacity;
using libchiaking::psn::rest::kamaji_title_id_capacity;
using libchiaking::psn::rest::StoreError;

class KamajiStore : public KamajiFixture {
 protected:
  static auto root_page_url(std::string_view segments) -> std::string {
    return std::string{store} + std::string{segments} +
           "?useOffers=true&gkb=1&gkb2=1&start=0&size=100";
  }
};

TEST_F(KamajiStore, CarriesTheSessionToStoresAndTheRootContainer) {
  opens_session({{"Set-Cookie", "JSESSIONID=JS1; Path=/"}});
  rig.fake.on(
      "GET", std::string{api} + "/user/stores",
      {Reply{
          200,
          R"({"header":{"status_code":"0x0000"},"data":{"base_url":"https://base/container/US/en/999/ROOT"}})"}});
  rig.fake.on(
      "GET", "https://base/container/US/en/999/ROOT?size=100",
      {Reply{200, R"({"links":[{"name":"A","url":"https://a"},"skip"]})"}});
  const auto base = client.stores();
  ASSERT_TRUE(base);
  EXPECT_EQ(base.value(), "https://base/container/US/en/999/ROOT");
  const auto root = client.container(base.value(), 100);
  ASSERT_TRUE(root);
  EXPECT_EQ(root.value().links.size(), 1U);
  EXPECT_FALSE(root.value().total_results);
  const Headers expected{{"User-Agent", stream_agent()},
                         {"Cookie", "JSESSIONID=JS1"},
                         {"Origin", "https://psnow.playstation.com"},
                         {"Referer", std::string{referer}},
                         {"Accept", "application/json"}};
  ASSERT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_TRUE(headers_match(rig.fake.seen.at(1), expected));
  EXPECT_TRUE(headers_match(rig.fake.seen.at(2), expected));
  EXPECT_EQ(rig.fake.seen.at(1).method, "GET");
  EXPECT_EQ(rig.fake.seen.at(2).method, "GET");
}

TEST_F(KamajiStore, ReportsRefusedAndEmptyStores) {
  rig.fake.on(
      "GET", std::string{api} + "/user/stores",
      {Reply{403, ""}, Reply{200, R"({"header":{"status_code":"0x0000"}})"}});
  const auto refused = client.stores();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(refused.error().http_status(), 403);
  const auto empty = client.stores();
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().reason(), StoreError::Reason::malformed);
  EXPECT_EQ(empty.error().http_status(), 200);
  EXPECT_EQ(rig.fake.seen.front().header("Cookie"), "JSESSIONID=");
}

TEST_F(KamajiStore, WalksAContainerPage) {
  rig.fake.on(
      "GET",
      std::string{store} +
          "US/en/19/"
          "STORE-MSF192018-APOLLOROOT?useOffers=true&gkb=1&gkb2=1&"
          "start=200&size=100",
      {Reply{200, R"({"total_results":250,"links":[{"id":"A"},{"id":"B"}]})"}});
  const auto page =
      client.container_page("US", "en", "STORE-MSF192018-APOLLOROOT", 200, 100);
  ASSERT_TRUE(page);
  EXPECT_EQ(page.value().total_results, 250);
  EXPECT_EQ(page.value().links.size(), 2U);
  EXPECT_TRUE(headers_match(
      rig.fake.seen.front(),
      {{"Accept", "application/json"}, {"User-Agent", stream_agent()}}));
}

TEST_F(KamajiStore, TruncatesALongContainerId) {
  const std::string id(600, 'A');
  EXPECT_FALSE(client.container_page("US", "en", id, 0, 100));
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& url = rig.fake.seen.front().url;
  const std::string prefix = std::string{store} + "US/en/19/";
  const std::string suffix = "?useOffers=true&gkb=1&gkb2=1&start=0&size=100";
  ASSERT_TRUE(url.starts_with(prefix)) << url;
  ASSERT_TRUE(url.ends_with(suffix)) << url;
  const auto segment =
      url.substr(prefix.size(), url.size() - prefix.size() - suffix.size());
  EXPECT_FALSE(segment.empty());
  EXPECT_LT(segment.size(), kamaji_title_id_capacity);
  EXPECT_EQ(segment, id.substr(0, segment.size()));
}

TEST_F(KamajiStore, EncodesTheContainerPageUrl) {
  EXPECT_FALSE(client.container_page("G B", "e/n", "STORE ROOT", 0, 100));
  ASSERT_FALSE(rig.fake.seen.empty());
  EXPECT_EQ(rig.fake.seen.front().url,
            root_page_url("G%20B/e%2Fn/19/STORE%20ROOT"));
}

TEST_F(KamajiStore, TellsAnUnparseablePageFromARefusedOne) {
  rig.fake.on("GET", root_page_url("US/en/19/ROOT"),
              {Reply{200, "not json"}, Reply{500, ""}});
  const auto broken = client.container_page("US", "en", "ROOT", 0, 100);
  ASSERT_FALSE(broken);
  EXPECT_EQ(broken.error().reason(), StoreError::Reason::malformed);
  const auto failed = client.container_page("US", "en", "ROOT", 0, 100);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(failed.error().http_status(), 500);
}

TEST_F(KamajiStore, EncodesTheTitleContainerUrl) {
  rig.fake.on(
      "GET",
      std::string{store} +
          "G%20B/en/19/EP0000-CUSA12345_00-TITLE?useOffers=true&gkb=1&gkb2=1",
      {Reply{404, ""}});
  const auto document = client.title_container<glz::raw_json>(
      "G B", "en", "EP0000-CUSA12345_00-TITLE");
  ASSERT_FALSE(document);
  EXPECT_EQ(document.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(document.error().http_status(), 404);
  EXPECT_TRUE(headers_match(rig.fake.seen.front(),
                            {{"Accept", "application/json"},
                             {"User-Agent", std::string{browser_agent}}}));
}

TEST_F(KamajiStore, CutsEachTitleContainerPathPartToItsBound) {
  const std::string country(100, 'C');
  const std::string language(100, 'L');
  const std::string title(600, 'T');
  const auto kept = [](std::size_t capacity) { return capacity - 4; };
  rig.fake.on("GET",
              std::string{store} +
                  country.substr(0, kept(kamaji_locale_component_capacity)) +
                  "/" +
                  language.substr(0, kept(kamaji_locale_component_capacity)) +
                  "/19/" + title.substr(0, kept(kamaji_title_id_capacity)) +
                  "?useOffers=true&gkb=1&gkb2=1",
              {Reply{404, ""}});
  const auto document =
      client.title_container<glz::raw_json>(country, language, title);
  ASSERT_FALSE(document);
  EXPECT_EQ(document.error().http_status(), 404);
}

TEST_F(KamajiStore, KeepsAnEscapeWholeWhenCuttingATitleId) {
  const auto title = std::string(250, 'T') + " X";
  rig.fake.on("GET",
              std::string{store} + "US/en/19/" + std::string(250, 'T') +
                  "%20?useOffers=true&gkb=1&gkb2=1",
              {Reply{404, ""}});
  const auto document =
      client.title_container<glz::raw_json>("US", "en", title);
  ASSERT_FALSE(document);
  EXPECT_EQ(document.error().http_status(), 404);
}

TEST_F(KamajiStore, AppendsThePageToACategory) {
  rig.fake.on("GET", "https://c/list?start=0&size=500", {Reply{200, "{}"}});
  rig.fake.on("GET", "https://c/list?x=1&start=500&size=500",
              {Reply{200, "{}"}});
  EXPECT_TRUE(client.category("https://c/list", 0, 500));
  EXPECT_TRUE(client.category("https://c/list?x=1", 500, 500));
  EXPECT_TRUE(headers_match(rig.fake.seen.front(),
                            {{"Content-Type", "application/json"},
                             {"Accept", "application/json"},
                             {"User-Agent", std::string{browser_agent}}}));
}

TEST_F(KamajiStore, SendsNothingOnceCancelled) {
  rig.stop();
  const auto stores = client.stores();
  ASSERT_FALSE(stores);
  ASSERT_TRUE(stores.error().failure());
  EXPECT_TRUE(stores.error().failure()->is_cancelled());
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace psn_test
