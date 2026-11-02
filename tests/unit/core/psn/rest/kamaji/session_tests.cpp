#include "tests/unit/core/psn/rest/kamaji/kamaji_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace psn_test {
namespace {

using Failure = libchiaking::net::http::Failure;

class KamajiSession : public KamajiFixture {};

TEST_F(KamajiSession, IsCreatedAndAdopted) {
  EXPECT_FALSE(client.has_session());
  opens_session({{"Content-Type", "application/json"},
                 {"set-cookie", "JSESSIONID=JS1; Path=/; Secure"}});
  EXPECT_TRUE(client.has_session());
  ASSERT_FALSE(rig.fake.seen.empty());
  const auto& seen = rig.fake.seen.front();
  EXPECT_EQ(seen.method, "POST");
  EXPECT_EQ(seen.url, std::string{api} + "/user/session");
  EXPECT_EQ(seen.body,
            "code=CODE1&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989&duid=" +
                std::string{duid});
  EXPECT_TRUE(headers_match(seen, {{"Content-Type", "text/plain;charset=UTF-8"},
                                   {"User-Agent", stream_agent()},
                                   {"X-Alt-Referer", std::string{alt_referer}},
                                   {"Origin", "https://psnow.playstation.com"},
                                   {"Referer", std::string{referer}},
                                   {"Accept", "*/*"}}));
  EXPECT_EQ(seen.total_timeout, libchiaking::net::http::default_total_timeout);
}

TEST_F(KamajiSession, IsNotAdoptedWithoutTheSessionCookie) {
  opens_session({{"Set-Cookie", "OTHER=1"}});
  EXPECT_FALSE(client.has_session());
}

TEST_F(KamajiSession, StillReportsTheStatusOfARefusal) {
  Reply refused{403, "{}"};
  refused.headers = {{"Set-Cookie", "JSESSIONID=ANON; Path=/"}};
  rig.fake.on("POST", std::string{api} + "/user/session", {refused});
  const auto created = client.create_session(code(), duid);
  ASSERT_TRUE(created);
  EXPECT_EQ(created.value().http_status, 403);
  EXPECT_FALSE(created.value().accepted);
  EXPECT_TRUE(client.has_session());
}

TEST_F(KamajiSession, IsNotRequestedWhenTheRequestIsOversized) {
  const std::string huge(500, 'x');
  const auto created = client.create_session(code(huge), duid);
  ASSERT_FALSE(created);
  ASSERT_TRUE(created.error().failure());
  EXPECT_EQ(created.error().failure()->kind(), Failure::Kind::invalid_request);
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace psn_test
