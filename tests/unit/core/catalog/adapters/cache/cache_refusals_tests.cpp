#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;
using Holding = libchiaking::catalog::StreamRefusal::Holding;

class CachedRefusals : public ::testing::Test, protected CacheCases {
 protected:
  static constexpr std::string_view name = "user/1111111111111111111/refusals";

  auto written() -> std::string {
    EXPECT_TRUE(shelf.cache.write(record));
    return shelf.text(name);
  }

  CacheShelf shelf;
  cache::RefusalRecord record{refusals(first_account, shelf.now())};
};

TEST_F(CachedRefusals, RoundTrip) {
  ASSERT_EQ(record.refusals().size(), 3U);
  ASSERT_TRUE(shelf.cache.write(record));
  const auto read = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  EXPECT_EQ(*read.value(), record);
  const auto& back = read.value()->refusals();
  ASSERT_EQ(back.size(), 3U);
  for (const auto& refusal : back) EXPECT_EQ(refusal.account(), first_account);
  EXPECT_EQ(back[0].circumstances().tier, Tier::none);
  EXPECT_EQ(back[1].circumstances().tier, Tier::premium);
  EXPECT_TRUE(back[1].questions_ownership());
  EXPECT_FALSE(back[2].circumstances().tier);
  EXPECT_EQ(back[2].refused_at(), shelf.now() - std::chrono::hours{3});
  EXPECT_EQ(back[0].circumstances().holding, Holding::unknown());
  ASSERT_TRUE(back[1].circumstances().holding.id());
  EXPECT_EQ(back[1].circumstances().holding.id()->text(), rivet);
  EXPECT_EQ(back[2].circumstances().holding, Holding::not_held());
}

TEST_F(CachedRefusals, AreWrittenUnderTheirSchema) {
  EXPECT_TRUE(written().starts_with(
      R"({"record":"refusals","schema":2,"writtenAtMs":1800000000000,)"));
}

TEST_F(CachedRefusals, AreWrittenByteForByte) {
  EXPECT_EQ(
      written(),
      R"({"record":"refusals","schema":2,"writtenAtMs":1800000000000,"account":)"
      R"("1111111111111111111","body":{"refusals":[{"product":"EP0106-PPSA07037)"
      R"(_00-FAKE0016TITLE000","kind":"refused_by_stream_server","refusedAtMs":)"
      R"(1800000000000,"tier":"none","ownership":"unknown"},{"product":"EP0109-)"
      R"(PPSA07028_00-FAKE0110TITLE000","kind":"no_game_for_entitlement","refus)"
      R"(edAtMs":1800000000000,"tier":"premium","ownership":"held","ownershipId)"
      R"(":"EP0109-PPSA07028_00-FAKE0110TITLE000"},{"product":"UP0114-PPSA37004)"
      R"(_00-FAKE0071TITLE000","kind":"refused_by_stream_server","refusedAtMs":)"
      R"(1799989200000,"ownership":"not_held"}]}})");
}

TEST_F(CachedRefusals, ErasesAnOlderSchema) {
  const auto text = written();
  shelf.put(name, replaced(text, R"("schema":2)", R"("schema":1)"));
  const auto older = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(older);
  EXPECT_FALSE(older.value());
  EXPECT_FALSE(shelf.has(name));
}

TEST_F(CachedRefusals, AreDroppedWhenAHeldOwnershipHasNoId) {
  const auto text = written();
  shelf.put(
      name,
      replaced(text, R"("ownershipId":"EP0109-PPSA07028_00-FAKE0110TITLE000")",
               R"("ownershipId":"")"));
  const auto unnamed = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(unnamed);
  EXPECT_FALSE(unnamed.value());
}

TEST_F(CachedRefusals, AreDroppedForAnUnknownOwnershipWord) {
  const auto text = written();
  shelf.put(name, replaced(text, R"("ownership":"not_held")",
                           R"("ownership":"rented")"));
  const auto unworded = shelf.cache.read<cache::RefusalRecord>(first_account);
  ASSERT_TRUE(unworded);
  EXPECT_FALSE(unworded.value());
}

TEST_F(CachedRefusals, AreNotMadeForAnotherAccount) {
  EXPECT_FALSE(cache::RefusalRecord::of(
      second_account, {record.refusals().begin(), record.refusals().end()}));
}

TEST_F(CachedRefusals, AreNotMadeWithARepeatedProduct) {
  EXPECT_FALSE(cache::RefusalRecord::of(
      first_account,
      {record.refusals()[0], record.refusals()[1], record.refusals()[0]}));
}

}  // namespace
}  // namespace catalog_test
