#include "libchiaking/testing/service_builder.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/check.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "src/core/users/ports/membership_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using namespace std::chrono_literals;
using libchiaking::AccountId;
using users::PsUser;
using users::SignInState;
using users::Unanswered;

class HomeUser : public ::testing::Test {
 protected:
  static constexpr AccountId tokyo{0x0807060504030201ULL};

  static auto signed_out(const auto& answer) -> testing::AssertionResult {
    if (answer) return testing::AssertionFailure() << "it was answered";
    if (answer.error().reason != Unanswered::Reason::signed_out)
      return testing::AssertionFailure()
             << "it was refused as "
             << testing::PrintToString(answer.error().reason);
    return testing::AssertionSuccess();
  }

  auto everyone() -> users::SignedInUsers& { return rig.signed_in.users; }

  auto store() -> libchiaking::testing::InMemoryCredentialStore& {
    return rig.signed_in.credentials;
  }

  SignedInRig rig;
};

TEST_F(HomeUser, NeedsAnAccountAndAName) {
  EXPECT_EQ(everyone().add_home_user(AccountId{0}, "Player").error(),
            PsUser::Invalid::account_id);
  EXPECT_EQ(everyone().add_home_user(AccountId{40}, "").error(),
            PsUser::Invalid::online_id);
  EXPECT_TRUE(everyone().everyone().empty());
  EXPECT_EQ(store().saves, 0);
}

TEST_F(HomeUser, IsSavedWithoutAnyCredential) {
  const auto added = everyone().add_home_user(tokyo, "Living Room Player");
  ASSERT_TRUE(added);
  const auto& home = added.value();
  EXPECT_EQ(home->account_id(), tokyo);
  EXPECT_EQ(home->online_id(), "Living Room Player");
  EXPECT_FALSE(home->has_psn_account());
  EXPECT_TRUE(home->is_saved());
  EXPECT_EQ(everyone().find(home->account_id()), home);
  EXPECT_EQ(home->sign_in_state(), SignInState::signed_out);
  EXPECT_FALSE(home->is_signed_in());
  EXPECT_FALSE(home->is_expired());
  EXPECT_FALSE(home->region());
  EXPECT_FALSE(home->avatar_url());
  EXPECT_FALSE(home->membership().is_ps_plus_member());
  EXPECT_EQ(store().saves, 1);
  EXPECT_FALSE(store().saved(home->account_id()));
  EXPECT_EQ(rig.signed_in.random.fills, 0);
  EXPECT_EQ(store().saved_home_user(home->account_id()),
            (users::SavedHomeUser{home->account_id(), "Living Room Player"}));
}

TEST_F(HomeUser, IsSignedOutOfEveryCallWithoutARequest) {
  const auto added = everyone().add_home_user(tokyo, "Living Room Player");
  ASSERT_TRUE(added);
  const auto& home = added.value();
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  users::MembershipCalls membership{*home};
  users::CatalogCalls catalog{*home};
  users::ConsoleCalls consoles{*home};
  EXPECT_TRUE(signed_out(home->get_my_profile()));
  EXPECT_TRUE(signed_out(home->get_my_membership()));
  EXPECT_TRUE(signed_out(home->stay_signed_in()));
  EXPECT_EQ(membership.membership(http).error().status(),
            psn::graphql::CheckStatus::no_session);
  EXPECT_TRUE(signed_out(catalog.old_service_store(http)));
  EXPECT_TRUE(signed_out(catalog.library_records(http)));
  EXPECT_TRUE(signed_out(catalog.library_record(http, "PROD")));
  EXPECT_TRUE(signed_out(consoles.account_consoles(http, 0)));
  EXPECT_TRUE(signed_out(consoles.installed_titles(http, 0)));
  EXPECT_TRUE(signed_out(catalog.played_titles(http, 0)));
  EXPECT_TRUE(
      signed_out(consoles.start_title(http, "duid", "PPSA00001_00", 5s)));
  EXPECT_TRUE(
      signed_out(consoles.title_start_progress(http, "CMD", false, "PS5", 5s)));
  EXPECT_TRUE(rig.fake.seen.empty());
  EXPECT_EQ(home->snapshot_json().value().find("Living Room"),
            std::string::npos);
}

TEST_F(HomeUser, ComesBackAfterARestartAndGivesWayToASignIn) {
  const auto added = everyone().add_home_user(tokyo, "Living Room Player");
  ASSERT_TRUE(added);
  const auto& home = added.value();
  users::SignedInUsers later{
      libchiaking::testing::ServiceBuilder::build<users::SignedInUsers>(
          rig.fake, store(), Runtime::threading(), rig.clock, rig.wall,
          rig.signed_in.random)};
  const auto returned = later.welcome_back();
  ASSERT_TRUE(returned);
  EXPECT_EQ(returned.value().rejected, 0U);
  ASSERT_EQ(returned.value().users.size(), 1U);
  const auto& back = returned.value().users.front();
  EXPECT_NE(back, home);
  EXPECT_EQ(back->account_id(), home->account_id());
  EXPECT_EQ(back->online_id(), "Living Room Player");
  EXPECT_FALSE(back->has_psn_account());
  EXPECT_TRUE(back->is_saved());
  EXPECT_EQ(back->sign_in_state(), SignInState::signed_out);
  EXPECT_EQ(store().saves, 1);
  EXPECT_TRUE(rig.fake.seen.empty());

  rig.answers_a_sign_in_as_tokyo_player();
  const auto signed_in = everyone().sign_in(
      psn::auth::Npsso{"synthetic-npsso"}, Psn::own_device(), Psn::correlation);
  ASSERT_TRUE(signed_in);
  EXPECT_NE(signed_in.value(), home);
  EXPECT_EQ(signed_in.value()->account_id(), home->account_id());
  const auto& full = signed_in.value();
  EXPECT_TRUE(full->has_psn_account());
  EXPECT_EQ(full->sign_in_state(), SignInState::signed_in);
  EXPECT_TRUE(full->is_saved());
  EXPECT_EQ(full->online_id(), "Tokyo Player");
  EXPECT_EQ(everyone().find(home->account_id()), full);
  EXPECT_EQ(everyone().everyone().size(), 1U);
  EXPECT_FALSE(home->has_psn_account());
  EXPECT_EQ(home->sign_in_state(), SignInState::signed_out);
  EXPECT_TRUE(store().saved(full->account_id()));
  EXPECT_FALSE(store().saved_home_user(full->account_id()));
  EXPECT_EQ(
      everyone().add_home_user(full->account_id(), "Someone Else").value(),
      full);
  EXPECT_TRUE(store().saved(full->account_id()));
  EXPECT_FALSE(store().saved_home_user(full->account_id()));

  const auto removals = store().removals;
  EXPECT_TRUE(later.sign_out(back->account_id()));
  EXPECT_EQ(store().removals, removals + 1);
  EXPECT_FALSE(later.find(back->account_id()));
  EXPECT_FALSE(store().saved(back->account_id()));
}

TEST_F(HomeUser, WithAnUnfitNameIsTurnedAwayOnReturn) {
  EXPECT_TRUE(store().save(users::SavedHomeUser{AccountId{41}, "bad\nname"}));
  EXPECT_TRUE(store().save(users::SavedHomeUser{AccountId{42}, "Guest"}));
  const auto sorted = everyone().welcome_back();
  ASSERT_TRUE(sorted);
  EXPECT_EQ(sorted.value().rejected, 1U);
  ASSERT_EQ(sorted.value().users.size(), 1U);
  EXPECT_EQ(sorted.value().users.front()->account_id(), AccountId{42});
}

}  // namespace
}  // namespace users_test
