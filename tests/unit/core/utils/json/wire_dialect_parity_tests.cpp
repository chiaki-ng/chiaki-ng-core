#include "tests/unit/core/utils/json/wire_dialect_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

namespace json_test {
namespace {

TEST(WireDialectParity, ReadsEveryPsnFixtureAsTheStrictReaderDoes) {
  std::size_t count = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(LIBCHIAKING_PSN_FIXTURES)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream stream{entry.path(), std::ios::binary};
    ASSERT_TRUE(stream) << entry.path();
    const std::string text{std::istreambuf_iterator<char>{stream}, {}};
    EXPECT_TRUE(WireDialectRig::matches_strict(text)) << entry.path();
    ++count;
  }
  EXPECT_GT(count, 0U);
}

TEST(WireDialectParity, ReadsGeneratedAndMutatedTextAsTheStrictReaderDoes) {
  std::minstd_rand random{4419};
  constexpr std::string_view mutations = "{}[]\"\\,:abc0123456789 -.\r\n";
  for (std::uint32_t index = 0; index < 2000; ++index) {
    auto text = std::format(
        R"({{"sequence":{},"nested":[true,null,"escaped\"\\",-5,0.25],)"
        R"("dataType":"psn:sessionManager:sys:rps:members:created"}})",
        index);
    if (index % 7 == 0) text = "true";
    if (index % 11 == 0) text = "null";
    if (index % 13 == 0) text = "[1,2,3]";
    EXPECT_TRUE(WireDialectRig::matches_strict(text)) << text;
    EXPECT_TRUE(WireDialectRig::matches_strict("\xEF\xBB\xBF" + text)) << text;
    EXPECT_TRUE(WireDialectRig::matches_strict(text + "trailing")) << text;
    auto mutated = text;
    const auto position =
        static_cast<std::size_t>(random()) % (mutated.size() + 1);
    const auto symbol =
        mutations[static_cast<std::size_t>(random()) % mutations.size()];
    mutated.insert(position, 1, symbol);
    EXPECT_TRUE(WireDialectRig::matches_strict(mutated)) << mutated;
    if (!text.empty()) {
      const auto prefix =
          std::string_view{text}.substr(0, position % text.size());
      EXPECT_TRUE(WireDialectRig::matches_strict(prefix)) << prefix;
    }
  }
}

}  // namespace
}  // namespace json_test
