#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <string>

namespace users_test {
namespace {

namespace users = libchiaking::users;
using libchiaking::AccountId;
using test_support::users::PatternedRandom;

TEST(DeviceIdentity, IsMadeOnceForASignInWithoutOneAndKeptForOneWithIt) {
  SignedInRig rig;
  const auto older = rig.signed_in.as(rig.saved(21, "NPSSO-A", "app-a"));
  EXPECT_EQ(rig.signed_in.random.fills, 1);
  EXPECT_EQ(rig.signed_in.credentials.saves, 2);
  EXPECT_TRUE(older->is_saved());
  const auto given = rig.signed_in.credentials.saved(AccountId{21});
  ASSERT_TRUE(given);
  ASSERT_TRUE(given->device);
  EXPECT_EQ(given->device->view(), PatternedRandom::device);
  EXPECT_EQ(given->npsso.view(), "NPSSO-A");
  ASSERT_TRUE(given->store);
  EXPECT_EQ(given->store->token.view(), "app-a");

  auto identified = rig.saved(22, "NPSSO-B", "app-b");
  identified.device = Psn::own_device();
  const auto newer = rig.signed_in.as(std::move(identified));
  EXPECT_EQ(rig.signed_in.random.fills, 1);
  EXPECT_EQ(rig.signed_in.credentials.saves, 3);
  EXPECT_TRUE(newer->is_saved());
  const auto loaded = rig.signed_in.credentials.saved(AccountId{22});
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(loaded->device);
  EXPECT_EQ(loaded->device->view(), Psn::duid);
  libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
  EXPECT_FALSE(users::CatalogCalls{*newer}.old_service_store(http));
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_NE(
      rig.fake.seen.front().url.find("&duid=" + std::string{Psn::duid} + "&"),
      std::string::npos)
      << rig.fake.seen.front().url;
}

TEST(DeviceIdentity, ThatCannotBeMadeTurnsTheSignInAway) {
  SignedInRig unlucky;
  unlucky.signed_in.random.failing = true;
  EXPECT_TRUE(unlucky.signed_in.credentials.save(
      unlucky.saved(23, "NPSSO-C", "app-c")));
  const auto nobody = unlucky.signed_in.users.welcome_back();
  ASSERT_TRUE(nobody);
  EXPECT_EQ(nobody.value().rejected, 1U);
  EXPECT_TRUE(nobody.value().users.empty());
  EXPECT_EQ(unlucky.signed_in.credentials.saves, 1);
  EXPECT_FALSE(unlucky.signed_in.users.find(AccountId{23}));
}

}  // namespace
}  // namespace users_test
