#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/trial_memory.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::AccountId;
using libchiaking::catalog::TrialMemory;
using libchiaking::catalog::cache::UsedTrialRecord;

class UsedTrials : public ::testing::Test, protected RuleCases {
 protected:
  static constexpr std::string_view lumen =
      "EP0107-PPSA17005_00-FAKE0037TITLE000";
  static constexpr std::string_view hail =
      "UP0113-PPSA37006_00-FAKE0126TITLE000";
  static constexpr AccountId first_account{1111111111111111111ULL};
  static constexpr AccountId second_account{2222222222222222222ULL};

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore store{threading};
  libchiaking::testing::ManualClock clock;
  libchiaking::catalog::cache::Cache cache{store, clock};
};

TEST_F(UsedTrials, StartEmptyAndHoldANotedTrial) {
  TrialMemory memory{cache, threading};
  const auto none = memory.used(first_account);
  ASSERT_TRUE(none);
  EXPECT_TRUE(none.value().products.empty());
  EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  const auto used = memory.used(first_account);
  ASSERT_TRUE(used);
  EXPECT_EQ(used.value().products, std::vector{product(lumen)});
  EXPECT_NE(used.value().revision, none.value().revision);
  EXPECT_TRUE(memory.is_used(first_account, product(lumen)));
  EXPECT_FALSE(memory.is_used(first_account, product(hail)));
  EXPECT_FALSE(memory.is_used(second_account, product(lumen)));
}

TEST_F(UsedTrials, AreRememberedAcrossRestartsWithoutExpiring) {
  {
    TrialMemory memory{cache, threading};
    EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  }
  clock.now_us += 400ULL * 24 * 60 * 60 * 1'000'000ULL;
  TrialMemory restarted{cache, threading};
  EXPECT_TRUE(restarted.is_used(first_account, product(lumen)));
  const auto stored = cache.read<UsedTrialRecord>(first_account);
  ASSERT_TRUE(stored);
  ASSERT_TRUE(stored.value());
  EXPECT_EQ(stored.value()->products().size(), 1U);
}

TEST_F(UsedTrials, KeepTheirRevisionWhenATrialIsNotedAgain) {
  TrialMemory memory{cache, threading};
  EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  EXPECT_TRUE(memory.note(first_account, product(hail), false));
  const auto before = memory.used(first_account).value().revision;
  EXPECT_TRUE(memory.note(first_account, product(hail), false));
  EXPECT_EQ(memory.used(first_account).value().revision, before);
}

TEST_F(UsedTrials, ForgetATrialTheStoreOffersAgain) {
  TrialMemory memory{cache, threading};
  EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  EXPECT_TRUE(memory.note(first_account, product(hail), false));
  const auto before = memory.used(first_account).value().revision;
  EXPECT_TRUE(memory.note(first_account, product(lumen), true));
  EXPECT_FALSE(memory.is_used(first_account, product(lumen)));
  EXPECT_NE(memory.used(first_account).value().revision, before);
}

TEST_F(UsedTrials, DropTheRecordOnceEveryTrialIsOfferedAgain) {
  TrialMemory memory{cache, threading};
  EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  EXPECT_TRUE(memory.note(first_account, product(hail), false));
  EXPECT_TRUE(memory.note(first_account, product(lumen), true));
  EXPECT_TRUE(memory.note(first_account, product(hail), true));
  const auto stored = cache.read<UsedTrialRecord>(first_account);
  ASSERT_TRUE(stored);
  EXPECT_FALSE(stored.value());
}

TEST_F(UsedTrials, AreDroppedForAForgottenAccountOnly) {
  TrialMemory memory{cache, threading};
  EXPECT_TRUE(memory.note(first_account, product(lumen), false));
  EXPECT_TRUE(memory.note(second_account, product(lumen), false));
  EXPECT_TRUE(memory.forget(first_account));
  EXPECT_FALSE(memory.is_used(first_account, product(lumen)));
  EXPECT_TRUE(memory.is_used(second_account, product(lumen)));
  TrialMemory restarted{cache, threading};
  EXPECT_FALSE(restarted.is_used(first_account, product(lumen)));
  EXPECT_TRUE(restarted.is_used(second_account, product(lumen)));
}

TEST_F(UsedTrials, RefuseAStoredRecordWithAnUnreadableProduct) {
  EXPECT_FALSE(UsedTrialRecord::from_body({{"not a product"}}, first_account));
}

TEST_F(UsedTrials, ReadAStoredRecordWithoutRepeats) {
  const auto record = UsedTrialRecord::from_body(
      {{std::string{hail}, std::string{lumen}, std::string{hail}}},
      first_account);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->products().size(), 2U);
  EXPECT_EQ(record->products().front(), product(lumen));
  EXPECT_EQ(record->body().products.size(), 2U);
}

}  // namespace
}  // namespace catalog_test
