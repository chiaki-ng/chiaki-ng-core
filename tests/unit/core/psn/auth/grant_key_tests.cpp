#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::auth::Grant;
using libchiaking::psn::auth::GrantKey;
using libchiaking::psn::auth::ResponseType;

class GrantKeys : public ::testing::Test {
 protected:
  static auto ordered(const GrantKey& left, const GrantKey& right)
      -> testing::AssertionResult {
    if (left < right || right < left) return testing::AssertionSuccess();
    return testing::AssertionFailure() << "neither key orders before the other";
  }

  const Grant owned = AuthRig::owned_grant();
  const Grant checkout{std::string{AuthRig::commerce_client},
                       std::string{AuthRig::checkout_scope},
                       ResponseType::token};
};

TEST_F(GrantKeys, IgnoreScopeOrderAndRepeats) {
  Grant reordered = owned;
  reordered.scope =
      "user:account.attributes.validate  kamaji:get_internal_entitlements "
      "user:account.attributes.validate";
  EXPECT_EQ(GrantKey::of(reordered), GrantKey::of(owned));
  EXPECT_FALSE(GrantKey::of(reordered) < GrantKey::of(owned));
  EXPECT_FALSE(GrantKey::of(owned) < GrantKey::of(reordered));
}

TEST_F(GrantKeys, DifferByResponseClientAndScope) {
  Grant code = owned;
  code.response = ResponseType::code;
  Grant client = owned;
  client.client_id = "other";
  for (const auto& other : {code, client, checkout}) {
    SCOPED_TRACE(other.scope);
    EXPECT_NE(GrantKey::of(other), GrantKey::of(owned));
    EXPECT_TRUE(ordered(GrantKey::of(other), GrantKey::of(owned)));
  }
}

TEST_F(GrantKeys, CoverOnlyANarrowerScopeOfTheSameKind) {
  Grant code = owned;
  code.response = ResponseType::code;
  EXPECT_TRUE(GrantKey::of(checkout).covers(GrantKey::of(owned)));
  EXPECT_FALSE(GrantKey::of(owned).covers(GrantKey::of(checkout)));
  EXPECT_FALSE(GrantKey::of(checkout).covers(GrantKey::of(code)));
}

TEST_F(GrantKeys, ListTheirScopesSortedAndUnique) {
  EXPECT_EQ(GrantKey::of(Grant{"c", "b a  b", ResponseType::token}).scopes(),
            (std::vector<std::string>{"a", "b"}));
}

}  // namespace
}  // namespace psn_test
