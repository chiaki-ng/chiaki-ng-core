#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace users_test {
namespace {

namespace users = libchiaking::users;
using libchiaking::AccountId;
using test_support::users::PatternedRandom;
using users::PsUser;
using users::SignInState;
using users::Unanswered;
using Reply = Psn::Reply;

class OldServiceStore : public ::testing::Test {
 protected:
  static auto asked(SignedInRig& rig, const std::shared_ptr<PsUser>& user) {
    libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
    return users::CatalogCalls{*user}.old_service_store(http);
  }

  static auto player(SignedInRig& rig) -> std::shared_ptr<PsUser> {
    return rig.signed_in.as(rig.saved(5, "NPSSO-A", "app-a"));
  }

  static auto session() -> Reply {
    Reply reply{
        200,
        R"({"header":{"status_code":"0x0000"},"data":{"country":"GB","language":"en"}})"};
    reply.headers = {{"Set-Cookie", "JSESSIONID=SESSION; Path=/"}};
    return reply;
  }

  void SetUp() override {
    SignedInRig first_probe;
    ASSERT_FALSE(asked(first_probe, player(first_probe)));
    ASSERT_FALSE(first_probe.fake.seen.empty());
    authorize_url = first_probe.fake.seen.front().url;
    SignedInRig second_probe;
    second_probe.fake.on(
        "GET", authorize_url,
        {Psn::redirect("https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                       "grc-response.html?code=CATALOG")});
    ASSERT_FALSE(asked(second_probe, player(second_probe)));
    ASSERT_EQ(second_probe.fake.seen.size(), 2U);
    session_url = second_probe.fake.seen.back().url;
  }

  void authorizes(SignedInRig& rig) const {
    rig.fake.on("GET", authorize_url,
                {Psn::redirect("https://psnow.playstation.com/app/2.2.0/133/"
                               "5cdcc037d/grc-response.html?code=CATALOG")});
  }

  std::string authorize_url;
  std::string session_url;
};

TEST_F(OldServiceStore, IsOpenedAsTheUserAndKeepsItsRegion) {
  SignedInRig rig;
  const auto user = player(rig);
  EXPECT_FALSE(user->region());
  authorizes(rig);
  rig.fake.on("POST", session_url, {session()});
  const auto store = asked(rig, user);
  ASSERT_TRUE(store);
  EXPECT_TRUE(store.value().has_session());
  EXPECT_EQ(rig.fake.seen.front().header("Cookie"), "npsso=NPSSO-A");
  EXPECT_NE(rig.fake.seen.front().url.find(
                "&duid=" + std::string{PatternedRandom::device} + "&"),
            std::string::npos)
      << rig.fake.seen.front().url;
  EXPECT_TRUE(rig.fake.seen.back().body.ends_with(
      "&duid=" + std::string{PatternedRandom::device}))
      << rig.fake.seen.back().body;
  EXPECT_EQ(user->region(), (users::Region{"GB", "en"}));
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
  const auto kept = rig.signed_in.credentials.saved(AccountId{5});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->region, (users::Region{"GB", "en"}));
}

TEST_F(OldServiceStore, ExpiresTheSignInWhenTheAuthorizationIsRefused) {
  SignedInRig refused;
  const auto lapsed = player(refused);
  refused.fake.on("GET", authorize_url, {Reply{200, "<html>"}});
  const auto nothing = asked(refused, lapsed);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason, Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(refused.fake.seen.size(), 1U);
  EXPECT_EQ(lapsed->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(lapsed->is_expired());
  EXPECT_FALSE(lapsed->region());
}

TEST_F(OldServiceStore, ReportsARefusalWhenTheSessionIsBarred) {
  SignedInRig elsewhere;
  const auto barred = player(elsewhere);
  authorizes(elsewhere);
  elsewhere.fake.on("POST", session_url,
                    {Reply{403, R"({"header":{"status_code":"0x0003"}})"}});
  const auto turned_away = asked(elsewhere, barred);
  ASSERT_FALSE(turned_away);
  EXPECT_EQ(turned_away.error().reason,
            Unanswered::Reason::old_service_refused);
  EXPECT_EQ(turned_away.error().http_status, 403);
  EXPECT_EQ(elsewhere.fake.seen.size(), 2U);
  EXPECT_EQ(barred->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(barred->is_expired());
  EXPECT_FALSE(barred->region());
}

TEST_F(OldServiceStore, ExpiresTheSignInWhenTheSessionIsNotAccepted) {
  SignedInRig unaccepted;
  const auto declined = player(unaccepted);
  authorizes(unaccepted);
  unaccepted.fake.on("POST", session_url,
                     {Reply{200, R"({"header":{"status_code":"0x0003"}})"}});
  const auto not_accepted = asked(unaccepted, declined);
  ASSERT_FALSE(not_accepted);
  EXPECT_EQ(not_accepted.error().reason, Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(not_accepted.error().http_status, 200);
  EXPECT_TRUE(declined->is_expired());
}

TEST_F(OldServiceStore, IsOpenedOnceForQuestionsInFlightTogether) {
  SignedInRig shared;
  const auto sharer = player(shared);
  authorizes(shared);
  const auto [first, second] = Runtime::asked_together(
      shared.fake, "POST", session_url, session(), false,
      [&](const libchiaking::platform::StopToken& stop) {
        libchiaking::net::http::Client http{shared.fake, Runtime::steady(),
                                            stop};
        return users::CatalogCalls{*sharer}.old_service_store(http);
      });
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_TRUE(first.value().has_session());
  EXPECT_TRUE(second.value().has_session());
  EXPECT_EQ(shared.fake.seen.size(), 2U);
  EXPECT_TRUE(asked(shared, sharer));
  EXPECT_GT(shared.fake.seen.size(), 2U);
}

}  // namespace
}  // namespace users_test
