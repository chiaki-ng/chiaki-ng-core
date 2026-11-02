#include "tests/unit/core/psn/rest/commerce/commerce_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::commerce_entitlements_page_limit;
using libchiaking::psn::rest::CommerceError;
using Outcome = libchiaking::Expected<std::uint16_t, CommerceError>;

class CommerceEntitlementPages : public CommerceFixture {};

TEST_F(CommerceEntitlementPages, FollowTheOffset) {
  rig.fake.on("GET", page_url(0), {Reply{200, page_of(300)}});
  rig.fake.on("GET", page_url(300), {Reply{200, page_of(2)}});
  std::vector<int> observed;
  const auto result = client.internal_entitlements(
      bearer(), [&observed](int page, const Outcome& outcome) {
        EXPECT_TRUE(outcome);
        if (outcome) {
          EXPECT_EQ(outcome.value(), 200);
        }
        observed.push_back(page);
      });
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().requests, 2);
  EXPECT_EQ(result.value().last_status, 200);
  EXPECT_EQ(result.value().items.size(), 302U);
  EXPECT_EQ(observed, (std::vector<int>{1, 2}));
  EXPECT_EQ(rig.waiter.waits, std::vector<int>{100});
  for (const auto& seen : rig.fake.seen) {
    ASSERT_EQ(seen.headers.size(), 2U);
    EXPECT_EQ(seen.headers[0].name, "Authorization");
    EXPECT_EQ(seen.headers[0].value, "Bearer BEARER-FAKE");
    EXPECT_EQ(seen.headers[1].name, "Accept");
    EXPECT_EQ(seen.headers[1].value, "application/json");
  }
}

TEST_F(CommerceEntitlementPages, StopAtTheLimit) {
  for (int page = 0; page < commerce_entitlements_page_limit; ++page)
    rig.fake.on("GET", page_url(page * 300), {Reply{200, page_of(300)}});
  const auto result = client.internal_entitlements(bearer());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason(), CommerceError::Reason::truncated);
  EXPECT_EQ(result.error().name(), "truncated");
  EXPECT_EQ(result.error().requests(), commerce_entitlements_page_limit);
}

TEST_F(CommerceEntitlementPages, ObserveARefusedPage) {
  rig.fake.on("GET", page_url(0), {Reply{200, page_of(300)}});
  rig.fake.on("GET", page_url(300), {Reply{401, ""}});
  std::vector<std::pair<int, std::uint16_t>> observed;
  const auto refused = client.internal_entitlements(
      bearer(), [&observed](int page, const Outcome& outcome) {
        observed.emplace_back(
            page, outcome ? outcome.value() : outcome.error().http_status());
      });
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason(), CommerceError::Reason::auth_refused);
  EXPECT_EQ(refused.error().requests(), 2);
  EXPECT_EQ(refused.error().http_status(), 401);
  EXPECT_EQ(observed,
            (std::vector<std::pair<int, std::uint16_t>>{{1, 200}, {2, 401}}));
}

TEST_F(CommerceEntitlementPages, ObserveAnUnreachableServer) {
  rig.fake.on("GET", page_url(0), {Reply::transport()});
  int failures = 0;
  const auto gone = client.internal_entitlements(
      bearer(), [&failures](int page, const Outcome& outcome) {
        EXPECT_EQ(page, 1);
        EXPECT_FALSE(outcome);
        if (!outcome) {
          EXPECT_TRUE(outcome.error().failure());
        }
        ++failures;
      });
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().reason(), CommerceError::Reason::unavailable);
  EXPECT_EQ(gone.error().http_status(), 0);
  EXPECT_EQ(failures, 1);
}

TEST_F(CommerceEntitlementPages, ReportAMalformedPage) {
  rig.fake.on("GET", page_url(0), {Reply{200, "not json"}});
  const auto malformed = client.internal_entitlements(bearer());
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error().reason(), CommerceError::Reason::malformed);
  EXPECT_EQ(malformed.error().http_status(), 200);
}

}  // namespace
}  // namespace psn_test
