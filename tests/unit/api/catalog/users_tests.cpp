#include "tests/unit/api/catalog/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_api_test {
namespace {

using CatalogUsers = CatalogApiTest;

class SignedOutUsers : public CatalogApiTest {
 protected:
  using Cause = users::Unanswered::Reason;

  template <class Reason>
  static auto needs_sign_in(const catalog::CallFailure<Reason>& error,
                            Cause cause) -> testing::AssertionResult {
    if (error.reason() != Reason::user)
      return testing::AssertionFailure() << "the reason is not the user";
    if (!error.cause()) return testing::AssertionFailure() << "no cause";
    if (error.cause()->reason != cause)
      return testing::AssertionFailure() << "another cause";
    if (!error.needs_sign_in())
      return testing::AssertionFailure() << "no sign-in is asked for";
    return testing::AssertionSuccess();
  }
};

TEST_F(CatalogUsers, NeverSeeEachOthersAnswers) {
  Rig rig;
  rig.hold_exclusions();
  const auto first = rig.user(1);
  const auto second = rig.user(2, users::Region{"GB", "en"});
  ps_as_usual(rig);
  library_pages(rig, {{200, owned_page({rivet_owned})}, {200, owned_page({})}});
  premium_includes(rig, {premium_offer(comet, "Comet"),
                         premium_offer(rivet, "Rivet & Bolt: Split Worlds")});
  membership_is(rig, "TIER_30");
  cloud_streaming_allowed(rig);
  storefront_says(rig, rivet, streams_notice);

  const auto mine = loaded(rig, *first);
  const auto theirs = loaded(rig, *second);
  EXPECT_EQ(mine->account(), AccountId{1});
  EXPECT_EQ(theirs->account(), AccountId{2});
  EXPECT_TRUE(game_of(*mine, rivet).is_owned());
  EXPECT_FALSE(game_of(*theirs, rivet).is_owned());
  EXPECT_EQ(rig.http.count(old_service_stores), 1);
  EXPECT_EQ(rig.http.count(owned_list), 2);
  EXPECT_TRUE(rig.has_record("user/1/library"));
  EXPECT_TRUE(rig.has_record("user/2/library"));
  EXPECT_EQ(rig.catalog.library_of(*first).value()->account(), AccountId{1});
  EXPECT_EQ(rig.catalog.library_of(*second).value()->account(), AccountId{2});
  EXPECT_EQ(rig.catalog.library_of(*second).value()->size(), 0U);
  EXPECT_EQ(rig.catalog.get_streamable_list(*first).value(),
            rig.catalog.get_streamable_list(*second).value());

  EXPECT_TRUE(rig.catalog.can_launch(*first, product(rivet)).is_allowed());
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*second, product(rivet))),
            LaunchRefusal::Kind::add_to_library);

  const auto answer =
      rig.catalog.check_title_against_storefront(*first, product(rivet));
  ASSERT_TRUE(answer);
  EXPECT_EQ(answer.value().account(), AccountId{1});
  EXPECT_EQ(rig.http.count(store_app), 1);
  const auto other =
      rig.catalog.check_title_against_storefront(*second, product(rivet));
  ASSERT_TRUE(other);
  EXPECT_EQ(other.value().account(), AccountId{2});
  EXPECT_EQ(rig.http.count(store_app), 2);
  EXPECT_TRUE(
      rig.catalog.check_title_against_storefront(*first, product(rivet)));
  EXPECT_EQ(rig.http.count(store_app), 2);

  rig.catalog.remember_refusal(*first, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_TRUE(game_of(*loaded(rig, *first), rivet).remembered_refusal());
  EXPECT_FALSE(game_of(*loaded(rig, *second), rivet).remembered_refusal());
  EXPECT_TRUE(rig.has_record("user/1/refusals"));
  EXPECT_FALSE(rig.has_record("user/2/refusals"));
  EXPECT_FALSE(rig.records.names().empty());
  for (const auto& name : rig.records.names()) {
    const auto kept = rig.records.read(name);
    ASSERT_TRUE(kept) << name.text();
    std::string text;
    for (const auto byte : kept.value().data)
      text.push_back(static_cast<char>(byte));
    for (const std::string_view secret : {"NPSSO", "TOKEN1", "CODE1", "JS1"})
      EXPECT_EQ(text.find(secret), std::string::npos)
          << name.text() << " keeps " << secret;
  }

  const auto walks = rig.http.count(old_service_stores);
  const auto access = rig.catalog.cloud_access_of(*first);
  ASSERT_TRUE(access);
  EXPECT_EQ(access.value().account(), AccountId{1});
  EXPECT_EQ(rig.http.count(old_service_stores), walks);
  const auto other_access = rig.catalog.cloud_access_of(*second);
  ASSERT_TRUE(other_access);
  EXPECT_EQ(other_access.value().account(), AccountId{2});
  EXPECT_EQ(rig.http.count(old_service_stores), walks + 1);
  EXPECT_TRUE(rig.catalog.cloud_access_of(*second));
  EXPECT_EQ(rig.http.count(old_service_stores), walks + 1);

  rig.catalog.forget(AccountId{1});
  EXPECT_FALSE(rig.has_record("user/1/library"));
  EXPECT_FALSE(rig.has_record("user/1/refusals"));
  EXPECT_TRUE(rig.has_record("user/2/library"));
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*first, product(rivet))),
            LaunchRefusal::Kind::not_listed);
  EXPECT_EQ(refusal_kind(rig.catalog.can_launch(*second, product(rivet))),
            LaunchRefusal::Kind::add_to_library);
  library_pages(rig, {{200, owned_page({rivet_owned})}});
  const auto back = loaded(rig, *first);
  EXPECT_EQ(rig.http.count(owned_list), 3);
  EXPECT_TRUE(game_of(*back, rivet).is_owned());
  EXPECT_FALSE(game_of(*back, rivet).remembered_refusal());
}

TEST_F(SignedOutUsers, AreAnsweredThatTheyAreSignedOutWithoutAskingPs) {
  Rig rig;
  ps_as_usual(rig);
  const auto home = rig.home_user();
  EXPECT_TRUE(needs_sign_in(rig.catalog.get_streamable_list(*home).error(),
                            Cause::signed_out));
  EXPECT_TRUE(
      needs_sign_in(rig.catalog.library_of(*home).error(), Cause::signed_out));
  EXPECT_TRUE(needs_sign_in(rig.catalog.process_library_and_list(*home).error(),
                            Cause::signed_out));
  EXPECT_TRUE(needs_sign_in(
      rig.catalog.check_title_against_storefront(*home, product(rivet)).error(),
      Cause::signed_out));
  EXPECT_TRUE(needs_sign_in(
      rig.catalog.recheck_ownership(*home, product(rivet)).error(),
      Cause::signed_out));
  EXPECT_TRUE(
      needs_sign_in(rig.catalog.store_page_of(*home, product(rivet)).error(),
                    Cause::signed_out));
  EXPECT_TRUE(needs_sign_in(rig.catalog.cloud_access_of(*home).error(),
                            Cause::signed_out));
  const auto held = rig.catalog.can_launch(*home, product(rivet));
  ASSERT_EQ(refusal_kind(held), LaunchRefusal::Kind::check_unavailable);
  ASSERT_TRUE(held.refusal()->cause());
  EXPECT_TRUE(needs_sign_in(*held.refusal()->cause(), Cause::signed_out));
  const auto cleared = rig.catalog.clear_for_launch(*home, product(rivet));
  ASSERT_EQ(refusal_kind(cleared), LaunchRefusal::Kind::check_unavailable);
  ASSERT_TRUE(cleared.refusal()->cause());
  EXPECT_TRUE(needs_sign_in(*cleared.refusal()->cause(), Cause::signed_out));
  rig.catalog.remember_refusal(*home, product(rivet),
                               StreamRefusal::Kind::refused_by_stream_server);
  EXPECT_FALSE(rig.catalog.explain(*home, product(rivet)).is_listed());
  EXPECT_EQ(rig.http.requests(), 0U);
  EXPECT_TRUE(rig.records.names().empty());
}

TEST_F(SignedOutUsers, WhoseSignInExpiredAreAnsweredThatItExpired) {
  Rig rig;
  ps_as_usual(rig);
  const auto user = rig.user();
  rig.http.on("GET", "response_type=code",
              {{302, "",
                test_support::catalog::ScriptedPs::location(
                    "https://x/grc-response.html?error=login_required")}});
  const auto refused = rig.catalog.process_library_and_list(*user);
  ASSERT_FALSE(refused);
  EXPECT_TRUE(needs_sign_in(refused.error(), Cause::sign_in_expired));
  EXPECT_TRUE(user->is_expired());
  EXPECT_EQ(rig.http.count(all_ps5_list), 0);
  EXPECT_EQ(rig.http.count(owned_list), 0);
  EXPECT_FALSE(rig.has_record("streamable/gb-en"));
  const auto asked = rig.http.requests();
  const auto still = rig.catalog.process_library_and_list(*user);
  ASSERT_FALSE(still);
  EXPECT_TRUE(needs_sign_in(still.error(), Cause::sign_in_expired));
  EXPECT_TRUE(needs_sign_in(rig.catalog.library_of(*user).error(),
                            Cause::sign_in_expired));
  EXPECT_TRUE(needs_sign_in(rig.catalog.cloud_access_of(*user).error(),
                            Cause::sign_in_expired));
  EXPECT_EQ(refusal_kind(rig.catalog.clear_for_launch(*user, product(rivet))),
            LaunchRefusal::Kind::check_unavailable);
  EXPECT_EQ(rig.http.requests(), asked);
}

}  // namespace
}  // namespace catalog_api_test
