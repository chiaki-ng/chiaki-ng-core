#include "tests/unit/api/accounts/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace accounts_test {
namespace {

using libchiaking::psn::Session;
using namespace std::string_view_literals;

class PsnSessionAppToken : public PsnSessionTest {
 protected:
  void SetUp() override {
    auto opened = open("NPSSO");
    ASSERT_TRUE(opened);
    ASSERT_TRUE(opened.value());
    session = std::move(opened.value());
  }

  static constexpr auto epoch = std::chrono::system_clock::time_point{};
  static constexpr auto later =
      epoch + std::chrono::milliseconds{4'102'444'800'000};

  std::shared_ptr<Session> session;
};

TEST_F(PsnSessionAppToken, RefusesAnEmptyOrSpacedToken) {
  for (const auto token : {""sv, "TO KEN"sv}) {
    const auto refused = session->supply_app_token(token, later);
    ASSERT_FALSE(refused) << token;
    EXPECT_EQ(refused.error(), Session::Error::invalid_app_token) << token;
  }
}

TEST_F(PsnSessionAppToken, AcceptsUpToItsLimitAndNoMore) {
  const std::string oversized(Session::app_token_limit + 1, 'a');
  const auto too_long = session->supply_app_token(oversized, later);
  ASSERT_FALSE(too_long);
  EXPECT_EQ(too_long.error(), Session::Error::invalid_app_token);
  EXPECT_FALSE(session->supply_app_token(std::string(8193, 'a'), later));
  EXPECT_TRUE(session->supply_app_token(std::string(8192, 'a'), later));
}

TEST_F(PsnSessionAppToken, RefusesAnExpiryBeforeTheEpoch) {
  const auto expired =
      session->supply_app_token("TOKEN", epoch - std::chrono::milliseconds{1});
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error(), Session::Error::invalid_expiry);
  EXPECT_TRUE(session->supply_app_token("TOKEN", epoch));
}

TEST_F(PsnSessionAppToken, IsSnapshottedWithoutItsSecretUntilCleared) {
  ASSERT_TRUE(session->supply_app_token("APP-TOKEN-SECRET", later));
  const auto snapshot = session->snapshot_json().value();
  EXPECT_EQ(snapshot.find("APP-TOKEN-SECRET"), std::string::npos) << snapshot;
  EXPECT_NE(snapshot.find(R"x("label":"app token (supplied)")x"),
            std::string::npos)
      << snapshot;
  EXPECT_NE(
      snapshot.find(
          R"("expiresAtMs":4102444800000,"expirySource":"app","state":"valid")"),
      std::string::npos)
      << snapshot;
  EXPECT_NE(snapshot.find(R"("subscription":null,"graphql":[])"),
            std::string::npos)
      << snapshot;
  session->clear_app_token();
  EXPECT_EQ(session->snapshot_json().value().find(R"("state":"valid")"),
            std::string::npos);
}

}  // namespace
}  // namespace accounts_test
