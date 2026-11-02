#include "src/core/net/transport/http/url.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace {

using libchiaking::net::http::Escape;
using libchiaking::net::http::Form;
using libchiaking::net::http::PercentEncoding;
using libchiaking::net::http::Query;

TEST(PercentEncodingComponent, KeepsUnreservedCharactersAndEscapesTheRest) {
  EXPECT_EQ(PercentEncoding::component("kamaji:get_internal_entitlements "
                                       "user:account.attributes.validate"),
            "kamaji%3Aget_internal_entitlements%20user%3Aaccount.attributes."
            "validate");
  EXPECT_EQ(
      PercentEncoding::component(
          "kamaji:commerce_native kamaji:commerce_container "
          "kamaji:lists kamaji:s2s.subscriptionsPremium.get"),
      "kamaji%3Acommerce_native%20kamaji%3Acommerce_container%20kamaji%3Alists%"
      "20kamaji%3As2s.subscriptionsPremium.get");
  EXPECT_EQ(PercentEncoding::component(
                "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                "grc-response.html"),
            "https%3A%2F%2Fpsnow.playstation.com%2Fapp%2F2.2.0%2F133%"
            "2F5cdcc037d%2Fgrc-response.html");
  EXPECT_EQ(PercentEncoding::component("a-_.~Z9 /+?&=#\xff"),
            "a-_.~Z9%20%2F%2B%3F%26%3D%23%FF");
}

TEST(PercentEncodingComponent, StopsAtAnEmbeddedNul) {
  EXPECT_EQ(PercentEncoding::component(std::string_view{"ab\0cd", 5}), "ab");
}

TEST(PercentEncodingComponent, LeavesEmptyTextEmpty) {
  EXPECT_TRUE(PercentEncoding::component("").empty());
}

TEST(PercentEncodingTruncated, StopsBeforeTheCapacity) {
  EXPECT_EQ(PercentEncoding::component_truncated("abcdefgh", 8), "abcd");
  EXPECT_EQ(PercentEncoding::component_truncated("ab cd", 9), "ab%20");
  EXPECT_TRUE(PercentEncoding::component_truncated("abc", 4).empty());
  EXPECT_TRUE(PercentEncoding::component_truncated("abc", 0).empty());
}

TEST(PercentEncodingTruncated, MatchesTheFullEncodingWhenItFits) {
  EXPECT_EQ(PercentEncoding::component_truncated("EP0109-CUSA17003_00", 256),
            PercentEncoding::component("EP0109-CUSA17003_00"));
}

TEST(PercentEncodingForm, UsesPlusForSpacesAndKeepsTheStar) {
  EXPECT_EQ(PercentEncoding::form("a b*c~"), "a+b*c%7E");
  EXPECT_EQ(PercentEncoding::form("{\"id\":\"x\"}"),
            "%7B%22id%22%3A%22x%22%7D");
  EXPECT_EQ(PercentEncoding::form("A-Z_0.9"), "A-Z_0.9");
}

TEST(PercentEncodingSecrets, AppendsAComponentIntoASecret) {
  libchiaking::SecretString secret;
  PercentEncoding::append(secret, "a b/c", Escape::component);
  EXPECT_EQ(secret.view(), "a%20b%2Fc");
}

TEST(PercentEncodingSecrets, AppendsAFormValueIntoASecret) {
  libchiaking::SecretString form;
  PercentEncoding::append(form, "a b/c", Escape::form);
  EXPECT_EQ(form.view(), "a+b%2Fc");
}

TEST(PercentEncodingSecrets, QueryAddsAParameterAfterTheFixedOnes) {
  Query query{"https://host.example/path?fixed=1"};
  query.add("next", "x y", Escape::form);
  EXPECT_EQ(query.view(), "https://host.example/path?fixed=1&next=x+y");
}

TEST(PercentEncodingSecrets, FormJoinsItsFieldsWithAmpersands) {
  Form body;
  EXPECT_TRUE(body.view().empty());
  body.add("k", "v").add("a*b", "c d");
  EXPECT_EQ(std::move(body).take().view(), "k=v&a*b=c+d");
}

}  // namespace
