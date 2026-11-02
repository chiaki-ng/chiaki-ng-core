#include "src/core/psn/session_internals.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using libchiaking::AccountId;
using users::PsUser;
using users::SavedSignIn;

static_assert(!std::is_default_constructible_v<PsUser>);
static_assert(!std::is_copy_constructible_v<PsUser> &&
              !std::is_move_constructible_v<PsUser>);
static_assert(!std::is_constructible_v<PsUser, AccountId, std::string,
                                       std::unique_ptr<psn::Session>>);
static_assert(!std::is_constructible_v<PsUser, SavedSignIn>);
static_assert(!std::is_copy_constructible_v<SavedSignIn>);

class SignedInUsersAdmission : public ::testing::Test {
 protected:
  auto rows() const -> std::vector<SavedSignIn> {
    auto no_account = good(0);
    auto no_name = good(11);
    no_name.online_id.clear();
    auto control_name = good(12);
    control_name.online_id = std::string{"a\nb"};
    auto long_name = good(13);
    long_name.online_id.assign(PsUser::online_id_limit + 1, 'a');
    SavedSignIn bad_npsso{AccountId{14}, "player",
                          psn::auth::Npsso{"has space"}};
    SavedSignIn empty_npsso{AccountId{15}, "player", psn::auth::Npsso{""}};
    auto bad_token = good(16);
    bad_token.store.emplace(psn::auth::AppToken{"has space"},
                            Psn::at_ms(Psn::wall_start_ms));
    auto empty_refresh = good(17);
    empty_refresh.store_refresh.emplace(psn::auth::AppRefreshToken{""});
    auto empty_remote = good(18);
    empty_remote.remote_play.emplace(psn::auth::BearerToken{""},
                                     Psn::at_ms(Psn::wall_start_ms));
    auto bad_region = good(19);
    bad_region.region = users::Region{"U S", "en"};
    auto regional = good(20);
    regional.region = users::Region{"GB", "en"};
    std::vector<SavedSignIn> all;
    for (auto* row : {&no_account, &no_name, &control_name, &long_name,
                      &bad_npsso, &empty_npsso, &bad_token, &empty_refresh,
                      &empty_remote, &bad_region, &regional})
      all.push_back(std::move(*row));
    return all;
  }

  auto good(std::uint64_t account) const -> SavedSignIn {
    return rig.saved(account, "NPSSO", "app");
  }

  SignedInRig rig;
};

TEST_F(SignedInUsersAdmission, NeverAdmitsAnInvalidUser) {
  auto& store = rig.signed_in.credentials;
  for (const auto& row : rows()) EXPECT_TRUE(store.save(row));
  const auto returned = rig.signed_in.users.welcome_back();
  ASSERT_TRUE(returned);
  EXPECT_EQ(returned.value().rejected, 10);
  ASSERT_EQ(returned.value().users.size(), 1U);
  const auto& user = returned.value().users.front();
  EXPECT_EQ(user->account_id(), AccountId{20});
  EXPECT_EQ(user->online_id(), "player");
  EXPECT_EQ(user->region(), (users::Region{"GB", "en"}));
  EXPECT_FALSE(user->avatar_url());
  EXPECT_FALSE(user->membership().tier());
  EXPECT_FALSE(user->membership().is_ps_plus_member().has_value());
  EXPECT_EQ(rig.signed_in.users.everyone().size(), 1U);
  for (const std::uint64_t account :
       {0U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U})
    EXPECT_FALSE(rig.signed_in.users.find(AccountId{account})) << account;
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(SignedInUsersAdmission, ReportsAnUnavailableStore) {
  for (const auto& row : rows())
    ASSERT_TRUE(rig.signed_in.credentials.save(row));
  rig.signed_in.credentials.unavailable = true;
  const auto unavailable = rig.signed_in.users.welcome_back();
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(), users::CredentialStore::Error::unavailable);
}

}  // namespace
}  // namespace users_test
