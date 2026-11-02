#include "src/core/utils/text/base64.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::AccountContext;

class PsUserAccountContext : public ::testing::Test {
 protected:
  static constexpr std::string_view claims =
      R"({"age":16,"legal_country":"GB","locale":"en-GB","is_child":true})";

  static auto token() -> std::string {
    const auto bytes = std::as_bytes(std::span{claims.data(), claims.size()});
    return "eyJhbGciOiJub25lIn0." +
           libchiaking::text::base64::encode(
               bytes, libchiaking::text::base64::Alphabet::url) +
           ".signature";
  }

  SignedInRig rig;
};

TEST_F(PsUserAccountContext, IsReadFromTheStoreTokenAndKeptWithTheAccount) {
  const auto user = rig.signed_in.as(rig.saved(5, "NPSSO-A", token()));
  const auto context = user->account_context();
  EXPECT_EQ(context.age(), 16);
  EXPECT_EQ(context.legal_country(), "GB");
  EXPECT_EQ(context.locale(), "en-GB");
  EXPECT_EQ(context.is_child(), true);
  const auto kept = rig.signed_in.credentials.saved(AccountId{5});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->context, context);
}

TEST_F(PsUserAccountContext, IsRestoredFromTheSavedSignIn) {
  const auto context =
      rig.signed_in.as(rig.saved(5, "NPSSO-A", token()))->account_context();
  SignedInRig restored;
  auto remembered = restored.saved(5, "NPSSO-A", "app-a");
  remembered.context = context;
  EXPECT_EQ(restored.signed_in.as(std::move(remembered))->account_context(),
            context);
}

TEST_F(PsUserAccountContext, IsEmptyWhenTheTokenStatesNothing) {
  EXPECT_EQ(
      rig.signed_in.as(rig.saved(5, "NPSSO-A", "app-a"))->account_context(),
      AccountContext{});
}

}  // namespace
}  // namespace users_test
