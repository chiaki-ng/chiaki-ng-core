#include "tests/unit/core/psn/rest/kamaji/kamaji_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::rest::StoreError;

class KamajiCheckout : public KamajiFixture {
 protected:
  static auto preview_with(int status, std::string body) {
    RestRig fresh;
    fresh.fake.on("POST", std::string{api} + "/user/checkout/buynow/preview",
                  {Reply{status, std::move(body)}});
    return libchiaking::psn::rest::KamajiClient{fresh.http}.buynow_preview(
        bearer(), "SKU-A");
  }
};

TEST_F(KamajiCheckout, SendsAcceptWithoutASession) {
  rig.fake.on("POST", std::string{api} + "/user/checkout/buynow/preview",
              {Reply{200, std::string{free_cart}}});
  rig.fake.on("POST", std::string{api} + "/user/checkout/buynow",
              {Reply{200, std::string{accepted}}});
  const auto preview = client.buynow_preview(bearer(), "SKU-A");
  ASSERT_TRUE(preview);
  EXPECT_TRUE(preview.value().free);
  EXPECT_EQ(preview.value().sku, "SKU-SERVER");
  EXPECT_TRUE(client.buynow(bearer(), "SKU-A"));
  ASSERT_EQ(rig.fake.seen.size(), 2U);
  const auto& sent = rig.fake.seen.at(0);
  EXPECT_EQ(sent.body, "sku=SKU-A");
  EXPECT_TRUE(headers_match(
      sent,
      {{"Authorization", "Bearer BEARER-FAKE"},
       {"User-Agent", stream_agent()},
       {"Accept", "application/json"},
       {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
       {"Accept", "application/json, text/javascript, */*; q=0.01"},
       {"X-Requested-With", "XMLHttpRequest"},
       {"Origin", "https://psnow.playstation.com"},
       {"Referer", std::string{referer}}}));
  const auto& buy = rig.fake.seen.at(1);
  EXPECT_EQ(buy.body, "sku=SKU-A&skipEmail=true");
  EXPECT_TRUE(headers_match(
      buy, {{"Authorization", "Bearer BEARER-FAKE"},
            {"User-Agent", stream_agent()},
            {"Accept", "application/json"},
            {"Accept", "application/json"},
            {"Content-Type", "application/x-www-form-urlencoded"}}));
}

TEST_F(KamajiCheckout, RefreshesTheSession) {
  opens_session({{"Set-Cookie", "JSESSIONID=FIRST; Path=/"}});
  Reply preview{200, std::string{free_cart}};
  preview.headers = {{"Set-Cookie", "JSESSIONID=REFRESHED; Path=/"}};
  rig.fake.on("POST", std::string{api} + "/user/checkout/buynow/preview",
              {preview});
  rig.fake.on("POST", std::string{api} + "/user/checkout/buynow",
              {Reply{200, std::string{accepted}}});
  EXPECT_TRUE(client.buynow_preview(bearer(), "SKU-A"));
  EXPECT_TRUE(client.buynow(bearer(), "SKU-SERVER"));
  ASSERT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_EQ(rig.fake.seen.at(1).header("Cookie"), "JSESSIONID=FIRST");
  EXPECT_EQ(rig.fake.seen.at(2).header("Cookie"), "JSESSIONID=REFRESHED");
  ASSERT_GT(rig.fake.seen.at(2).headers.size(), 2U);
  EXPECT_EQ(rig.fake.seen.at(2).headers.at(2).name, "Cookie");
}

TEST_F(KamajiCheckout, PreviewReadsATextPrice) {
  const auto priced = preview_with(
      200,
      R"({"data":{"cart":{"total_price_value":"4.99","total_price":"$4.99"}}})");
  ASSERT_TRUE(priced);
  EXPECT_FALSE(priced.value().free);
  EXPECT_EQ(priced.value().display_price, "$4.99");
}

TEST_F(KamajiCheckout, PreviewReadsANumericPrice) {
  const auto numeric = preview_with(
      200,
      R"({"data":{"cart":{"total_price_value":1999,"total_price":1999}}})");
  ASSERT_TRUE(numeric);
  EXPECT_FALSE(numeric.value().free);
  EXPECT_EQ(numeric.value().display_price, "1999");
}

TEST_F(KamajiCheckout, PreviewReadsAnAbsentPriceAsFree) {
  const auto absent = preview_with(200, R"({"data":{"cart":{"items":[]}}})");
  ASSERT_TRUE(absent);
  EXPECT_TRUE(absent.value().free);
  EXPECT_TRUE(absent.value().sku.empty());
}

TEST_F(KamajiCheckout, PreviewIsRefusedByAnErrorStatusCode) {
  const auto refused = preview_with(
      200, R"({"header":{"status_code":"0x8000"},"data":{"cart":{}}})");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(refused.error().http_status(), 200);
}

TEST_F(KamajiCheckout, PreviewIsRefusedByAForbiddenReply) {
  const auto forbidden = preview_with(403, "{}");
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().reason(), StoreError::Reason::refused);
  EXPECT_EQ(forbidden.error().http_status(), 403);
}

TEST_F(KamajiCheckout, PreviewRejectsAMalformedCart) {
  for (const std::string_view body :
       {R"({"data":{"cart":[]}})",
        R"({"data":{"cart":{"total_price_value":"0 "}}})",
        R"({"data":{"cart":{"total_price_value":true}}})"}) {
    const auto malformed = preview_with(200, std::string{body});
    ASSERT_FALSE(malformed) << body;
    EXPECT_EQ(malformed.error().reason(), StoreError::Reason::malformed)
        << body;
  }
}

TEST_F(KamajiCheckout, BuyNowRequiresAnAcceptedBody) {
  rig.fake.on("POST", std::string{api} + "/user/checkout/buynow",
              {Reply{200, "{}"}, Reply{500, std::string{accepted}}});
  const auto unaccepted = client.buynow(bearer(), "SKU");
  ASSERT_FALSE(unaccepted);
  EXPECT_EQ(unaccepted.error().http_status(), 200);
  const auto failed = client.buynow(bearer(), "SKU");
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 500);
}

}  // namespace
}  // namespace psn_test
