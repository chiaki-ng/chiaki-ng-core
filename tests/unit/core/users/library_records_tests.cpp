#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using users::PsUser;
using users::SignInState;
using users::Unanswered;
using Reply = Psn::Reply;

class LibraryRecord : public ::testing::Test {
 protected:
  static auto asked(SignedInRig& rig, const std::shared_ptr<PsUser>& user) {
    libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
    return users::CatalogCalls{*user}.library_record(http, "PROD");
  }

  static auto player(SignedInRig& rig) -> std::shared_ptr<PsUser> {
    return rig.signed_in.as(rig.saved(6, "NPSSO-A", "app-a"));
  }

  void SetUp() override {
    SignedInRig first_probe;
    ASSERT_FALSE(asked(first_probe, player(first_probe)));
    ASSERT_FALSE(first_probe.fake.seen.empty());
    authorize_url = first_probe.fake.seen.front().url;
    SignedInRig second_probe;
    second_probe.fake.on("GET", authorize_url,
                         {Psn::redirect("https://x/#access_token=OWNED")});
    static_cast<void>(asked(second_probe, player(second_probe)));
    ASSERT_EQ(second_probe.fake.seen.size(), 2U);
    record_url = second_probe.fake.seen.back().url;
  }

  void authorizes(SignedInRig& rig) const {
    rig.fake.on("GET", authorize_url,
                {Psn::redirect("https://x/#access_token=OWNED")});
  }

  std::string authorize_url;
  std::string record_url;
};

TEST_F(LibraryRecord, IsAskedAsTheUserAndRenewedOnceRefused) {
  SignedInRig rig;
  const auto user = player(rig);
  authorizes(rig);
  rig.fake.on("GET", record_url,
              {Reply{200, R"({"id":"PROD"})"}, Reply{401, "{}"},
               Reply{200, R"({"id":"PROD"})"}});
  const auto owned = asked(rig, user);
  ASSERT_TRUE(owned);
  EXPECT_TRUE(owned.value().owned);
  EXPECT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer OWNED");
  const auto renewed = asked(rig, user);
  ASSERT_TRUE(renewed);
  EXPECT_TRUE(renewed.value().owned);
  EXPECT_EQ(renewed.value().http_status, 200);
  EXPECT_EQ(rig.fake.seen.size(), 5U);
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
}

TEST_F(LibraryRecord, ExpiresTheSignInWhenTheRenewedAccessIsRefused) {
  SignedInRig refused;
  const auto lapsed = player(refused);
  authorizes(refused);
  refused.fake.on("GET", record_url, {Reply{401, "{}"}});
  const auto nothing = asked(refused, lapsed);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason, Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(nothing.error().http_status, 401);
  EXPECT_EQ(lapsed->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(lapsed->is_expired());
}

TEST_F(LibraryRecord, IsAskedOnceForQuestionsInFlightTogether) {
  SignedInRig shared;
  const auto sharer = player(shared);
  authorizes(shared);
  const auto [first, second] = Runtime::asked_together(
      shared.fake, "GET", record_url, Reply{200, R"({"id":"PROD"})"}, false,
      [&](const libchiaking::platform::StopToken& stop) {
        libchiaking::net::http::Client http{shared.fake, Runtime::steady(),
                                            stop};
        return users::CatalogCalls{*sharer}.library_record(http, "PROD");
      });
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_TRUE(first.value().owned);
  EXPECT_TRUE(second.value().owned);
  EXPECT_EQ(shared.fake.seen.size(), 2U);
  EXPECT_TRUE(asked(shared, sharer));
  EXPECT_EQ(shared.fake.seen.size(), 3U);
}

class LibraryRecords : public ::testing::Test {
 protected:
  static constexpr std::string_view records_url =
      "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/"
      "internal_entitlements";

  static auto records_page_url(int start) -> std::string {
    return std::string{records_url} +
           "?fields=game_meta%2Csku_meta&entitlement_type=5&start=" +
           std::to_string(start) + "&size=300";
  }

  static auto records_page(int count) -> std::string {
    std::string body = R"({"entitlements":[)";
    for (int index = 0; index < count; ++index)
      body += index != 0 ? ",{}" : "{}";
    return body + "]}";
  }

  static auto asked(SignedInRig& rig, const std::shared_ptr<PsUser>& user,
                    std::vector<int>* pages = nullptr) {
    test_support::FakeWaiter waiter;
    libchiaking::net::http::Client http{
        rig.fake, Runtime::steady(), {}, nullptr, &waiter};
    return users::CatalogCalls{*user}.library_records(
        http, [pages](int page,
                      const libchiaking::Expected<std::uint16_t,
                                                  psn::rest::CommerceError>&) {
          if (pages != nullptr) pages->push_back(page);
        });
  }

  static auto player(SignedInRig& rig) -> std::shared_ptr<PsUser> {
    return rig.signed_in.as(rig.saved(8, "NPSSO-A", "app-a"));
  }

  void SetUp() override {
    SignedInRig probe;
    ASSERT_FALSE(asked(probe, player(probe)));
    ASSERT_FALSE(probe.fake.seen.empty());
    authorize_url = probe.fake.seen.front().url;
  }

  std::string authorize_url;
};

TEST_F(LibraryRecords, ArePagedAsTheUserAndRenewedOnceRefused) {
  SignedInRig rig;
  const auto user = player(rig);
  rig.fake.on("GET", authorize_url,
              {Psn::redirect("https://x/#access_token=OWNED"),
               Psn::redirect("https://x/#access_token=RENEWED")});
  rig.fake.on("GET", records_page_url(0),
              {Reply{200, records_page(300)}, Reply{401, "{}"},
               Reply{200, records_page(300)}});
  rig.fake.on("GET", records_page_url(300), {Reply{200, records_page(2)}});
  std::vector<int> pages;
  const auto owned = asked(rig, user, &pages);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned.value().items.size(), 302U);
  EXPECT_EQ(owned.value().requests, 2);
  EXPECT_EQ(owned.value().last_status, 200);
  ASSERT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(pages, (std::vector<int>{1, 2}));
  EXPECT_EQ(rig.fake.seen[1].url, records_page_url(0));
  EXPECT_EQ(rig.fake.seen[2].url, records_page_url(300));
  EXPECT_EQ(rig.fake.seen.back().header("Authorization"), "Bearer OWNED");

  const auto renewed = asked(rig, user);
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value().items.size(), 302U);
  EXPECT_EQ(renewed.value().requests, 2);
  ASSERT_EQ(rig.fake.seen.size(), 7U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.fake.seen[3].url, records_page_url(0));
  EXPECT_EQ(rig.fake.seen[4].url, authorize_url);
  EXPECT_EQ(rig.fake.seen[3].header("Authorization"), "Bearer OWNED");
  EXPECT_EQ(rig.fake.seen[5].header("Authorization"), "Bearer RENEWED");
  EXPECT_EQ(rig.fake.seen[6].header("Authorization"), "Bearer RENEWED");
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
}

TEST_F(LibraryRecords, ExpireTheSignInWhenTheRenewedAccessIsRefused) {
  SignedInRig refused;
  const auto lapsed = player(refused);
  refused.fake.on("GET", authorize_url,
                  {Psn::redirect("https://x/#access_token=OWNED")});
  refused.fake.on("GET", records_page_url(0),
                  {Reply{200, records_page(2)}, Reply{401, "{}"}});
  EXPECT_TRUE(asked(refused, lapsed));
  EXPECT_EQ(refused.fake.seen.size(), 2U);
  const auto nothing = asked(refused, lapsed);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason, Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(nothing.error().http_status, 401);
  ASSERT_EQ(refused.fake.seen.size(), 5U);
  EXPECT_EQ(refused.fake.seen[3].url, authorize_url);
  EXPECT_EQ(refused.fake.seen[2].url, records_page_url(0));
  EXPECT_EQ(refused.fake.seen[4].url, records_page_url(0));
  EXPECT_EQ(lapsed->sign_in_state(), SignInState::expired);
  EXPECT_TRUE(lapsed->is_expired());
}

TEST_F(LibraryRecords, AreAskedOnceForQuestionsInFlightTogether) {
  SignedInRig shared;
  const auto sharer = player(shared);
  shared.fake.on("GET", authorize_url,
                 {Psn::redirect("https://x/#access_token=OWNED")});
  const auto [first, second] = Runtime::asked_together(
      shared.fake, "GET", records_page_url(0), Reply{200, records_page(2)},
      false, [&](const libchiaking::platform::StopToken& stop) {
        libchiaking::net::http::Client http{shared.fake, Runtime::steady(),
                                            stop};
        return users::CatalogCalls{*sharer}.library_records(http);
      });
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value().items.size(), 2U);
  EXPECT_EQ(second.value().items.size(), 2U);
  EXPECT_EQ(shared.fake.seen.size(), 2U);
}

}  // namespace
}  // namespace users_test
