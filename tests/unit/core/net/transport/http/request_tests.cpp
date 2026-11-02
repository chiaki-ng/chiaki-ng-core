#include "tests/unit/core/net/transport/http/client_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace http_test {
namespace {

using libchiaking::net::http::Form;
using libchiaking::net::http::HeaderCredential;
using libchiaking::net::http::Query;
using libchiaking::net::http::Request;
using libchiaking::net::http::Response;
using test_support::psn::Reply;

static_assert(HeaderCredential<TestBearer>);
static_assert(HeaderCredential<TestCookie>);
static_assert(!HeaderCredential<TestCode>);
static_assert(!std::is_copy_constructible_v<Request>);
static_assert(!std::is_copy_constructible_v<Response>);
static_assert(!std::is_copy_constructible_v<TestBearer>);

class HttpRequest : public ClientRig {};

TEST_F(HttpRequest, KeepsHeaderOrderAndTheCredentialLabel) {
  fake.on("POST", std::string{url}, {Reply{200, "{}"}});
  Form form;
  form.add("grant type", "code").add("code", "A&B=C");
  Request request{posted, url};
  request.header("User-Agent", "agent/1")
      .authorize(TestCookie{"NPSSO-1"})
      .form(std::move(form));
  EXPECT_EQ(request.credential_label(), "test cookie");
  EXPECT_EQ(request.headers().size(), 3U);
  EXPECT_EQ(request.body(), "grant+type=code&code=A%26B%3DC");
  EXPECT_TRUE(client.send(std::move(request)));
  ASSERT_FALSE(fake.seen.empty());
  const auto& seen = fake.seen.front();
  ASSERT_EQ(seen.headers.size(), 3U);
  EXPECT_EQ(seen.headers[0].name, "User-Agent");
  EXPECT_EQ(seen.headers[1].name, "Cookie");
  EXPECT_EQ(seen.headers[1].value, "npsso=NPSSO-1");
  EXPECT_EQ(seen.headers[2].name, "Content-Type");
  EXPECT_EQ(seen.headers[2].value, "application/x-www-form-urlencoded");
  EXPECT_EQ(seen.body, "grant+type=code&code=A%26B%3DC");
  EXPECT_TRUE(log.contains("auth=test cookie"));
}

TEST_F(HttpRequest, CarriesTheEndpointsExpectedStatuses) {
  static constexpr std::array<std::uint16_t, 2> answers{400, 404};
  libchiaking::net::http::Endpoint endpoint = plain;
  endpoint.expected_statuses = answers;
  fake.on("GET", std::string{url}, {Reply{404, "{}"}});
  EXPECT_TRUE(client.send(Request{endpoint, url}));
  ASSERT_FALSE(fake.seen.empty());
  EXPECT_EQ(fake.seen.front().expected_statuses,
            (std::vector<std::uint16_t>{400, 404}));
}

TEST_F(HttpRequest, TakesAQueryBuiltUrl) {
  const std::string expected = at("?scope=a%20b&raw=x y");
  fake.on("GET", expected, {Reply{200, "{}"}});
  Query query{url};
  query.add("scope", "a b").add_verbatim("raw", "x y");
  Request request{plain, std::move(query)};
  EXPECT_EQ(request.url(), expected);
  EXPECT_TRUE(client.send(std::move(request)));
}

TEST(HttpCredential, ClonesIntoIndependentStorage) {
  const TestBearer original{"bearer-value"};
  const auto copy = original.clone();
  EXPECT_EQ(copy.view(), "bearer-value");
  EXPECT_NE(copy.secret().data(), original.secret().data());
  EXPECT_EQ(copy.label(), "test bearer");
  EXPECT_FALSE(copy.empty());
}

TEST(HttpCredential, TakesALabelOverTheKindDefault) {
  const TestCode code{libchiaking::SecretString{"code-1"}, "relabelled"};
  EXPECT_EQ(code.label(), "relabelled");
  EXPECT_EQ(code.view(), "code-1");
  EXPECT_EQ(TestCode{"code-2"}.label(), "test code");
}

}  // namespace
}  // namespace http_test
