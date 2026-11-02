#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string_view>

namespace chiaki_ng_test {
namespace {

using ChiakiNgCaches = ChiakiNgTest;

TEST_F(ChiakiNgCaches, AreInspectedAndClearedFromTheMainClass) {
  Household home;
  const auto empty = home.chiaki.caches();
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty.value().size(), catalog::every_cache.size());
  for (const auto& summary : empty.value()) {
    EXPECT_EQ(summary.entries, 0U);
    EXPECT_EQ(summary.bytes, 0U);
  }

  const std::string_view text = "{}";
  const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
  for (const auto name : {"streamable/us-en", "included/en-us",
                          "for_sale/en-us", "user/40/library"})
    EXPECT_TRUE(
        home.records.write_atomic(*platform::RecordName::parse(name), bytes));
  const auto held = home.chiaki.caches();
  ASSERT_TRUE(held);
  for (const auto& summary : held.value()) {
    const bool written = summary.id == catalog::CacheId::streamable_lists ||
                         summary.id == catalog::CacheId::included_titles ||
                         summary.id == catalog::CacheId::for_sale_titles ||
                         summary.id == catalog::CacheId::libraries;
    EXPECT_EQ(summary.entries, written ? 1U : 0U)
        << ::testing::PrintToString(summary.id);
    EXPECT_EQ(summary.bytes, written ? 2U : 0U)
        << ::testing::PrintToString(summary.id);
  }
  const auto libraries = home.chiaki.cache_entries(catalog::CacheId::libraries);
  ASSERT_TRUE(libraries);
  ASSERT_EQ(libraries.value().size(), 1U);
  EXPECT_EQ(libraries.value().front().bytes, 2U);

  EXPECT_TRUE(home.chiaki.clear_cache(catalog::CacheId::libraries));
  const auto without = home.chiaki.cache_entries(catalog::CacheId::libraries);
  ASSERT_TRUE(without);
  EXPECT_TRUE(without.value().empty());
  EXPECT_EQ(home.records.names().size(), 3U);
  EXPECT_TRUE(home.chiaki.clear_caches());
  EXPECT_TRUE(home.records.names().empty());
}

}  // namespace
}  // namespace chiaki_ng_test
