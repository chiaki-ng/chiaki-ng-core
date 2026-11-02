#include "src/core/utils/json/ps_fields.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string_view>

namespace json_test {
namespace {

using libchiaking::json::ps::Elapsed;
using std::chrono::seconds;

class PlayTime : public ::testing::Test {
 protected:
  static auto length_of(std::string_view text) -> std::optional<seconds> {
    const auto read = Elapsed::from_text(text);
    if (!read) return std::nullopt;
    return read->length();
  }
};

TEST_F(PlayTime, ReadsAsPsWritesIt) {
  EXPECT_EQ(length_of("PT12H34M5S"), seconds{12 * 3600 + 34 * 60 + 5});
  EXPECT_EQ(length_of("PT47H12M"), seconds{47 * 3600 + 12 * 60});
  EXPECT_EQ(length_of("PT9H"), seconds{9 * 3600});
  EXPECT_EQ(length_of("PT228H56M33S"), seconds{228 * 3600 + 56 * 60 + 33});
  EXPECT_EQ(length_of("PT45S"), seconds{45});
  EXPECT_EQ(length_of("PT0S"), seconds{0});
}

TEST_F(PlayTime, ReadsDaysWeeksAnyCaseAndFractions) {
  EXPECT_EQ(length_of("P1DT2H"), seconds{26 * 3600});
  EXPECT_EQ(length_of("P2D"), seconds{2 * 86400});
  EXPECT_EQ(length_of("P1W"), seconds{7 * 86400});
  EXPECT_EQ(length_of("pt1h30m"), seconds{5400});
  EXPECT_EQ(length_of("PT1.75S"), seconds{1});
  EXPECT_EQ(length_of("PT1M0,5S"), seconds{60});
}

TEST_F(PlayTime, RefusesMalformedDurations) {
  for (const auto text :
       {"",      "P",     "PT",      "T1H",   "12H",    "PT1H2",  "PTH",
        "PT1X",  "P1Y",   "P1M",     "P1DT",  "PT1H1H", "PT1S1M", "PT1.5H",
        "PT1.S", "P1D1W", "PT1HT1M", "PT-1H", "PT 1H",  "PT1H ",  "yesterday"})
    EXPECT_FALSE(Elapsed::from_text(text)) << '"' << text << '"';
}

TEST_F(PlayTime, RefusesADurationTooLongToHold) {
  EXPECT_FALSE(Elapsed::from_text("PT99999999999999999999S"));
  EXPECT_FALSE(Elapsed::from_text("P9999999999999999W"));
  EXPECT_EQ(length_of("PT9223372036854775807S"),
            seconds{9223372036854775807LL});
  EXPECT_FALSE(Elapsed::from_text("PT9223372036854775808S"));
}

}  // namespace
}  // namespace json_test
