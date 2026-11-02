#include "src/core/utils/text/ascii.hpp"

#include <gtest/gtest.h>

#include <compare>
#include <string>
#include <string_view>

namespace text_test {
namespace {

using namespace std::string_view_literals;

namespace text = libchiaking::text;

TEST(AsciiClasses, NeverCountHighBitBytesAsLettersDigitsOrSpace) {
  for (int value = 0x80; value <= 0xff; ++value) {
    const auto byte = static_cast<char>(value);
    EXPECT_FALSE(text::ascii_digit(byte)) << value;
    EXPECT_FALSE(text::ascii_alnum(byte)) << value;
    EXPECT_FALSE(text::ascii_space(byte)) << value;
    EXPECT_FALSE(text::ascii_control(byte)) << value;
    EXPECT_FALSE(text::ascii_visible(byte)) << value;
    EXPECT_FALSE(text::ascii_printable(byte)) << value;
    EXPECT_EQ(text::ascii_lower(byte), byte) << value;
    EXPECT_EQ(text::ascii_upper(byte), byte) << value;
  }
}

TEST(AsciiClasses, SortEverySevenBitByte) {
  for (int value = 0; value < 0x80; ++value) {
    const auto byte = static_cast<char>(value);
    const bool digit = value >= '0' && value <= '9';
    const bool lower = value >= 'a' && value <= 'z';
    const bool upper = value >= 'A' && value <= 'Z';
    EXPECT_EQ(text::ascii_digit(byte), digit) << value;
    EXPECT_EQ(text::ascii_alnum(byte), digit || lower || upper) << value;
    EXPECT_EQ(text::ascii_control(byte), value < 0x20 || value == 0x7f)
        << value;
    EXPECT_EQ(text::ascii_visible(byte), value > 0x20 && value < 0x7f) << value;
    EXPECT_EQ(text::ascii_printable(byte), value >= 0x20 && value < 0x7f)
        << value;
    EXPECT_EQ(text::ascii_lower(byte),
              upper ? static_cast<char>(value + 0x20) : byte)
        << value;
    EXPECT_EQ(text::ascii_upper(byte),
              lower ? static_cast<char>(value - 0x20) : byte)
        << value;
  }
}

TEST(AsciiClasses, CountVerticalTabAndFormFeedAsSpaceButNotNul) {
  for (const char space : " \t\n\v\f\r"sv)
    EXPECT_TRUE(text::ascii_space(space));
  for (const char other : "\0\x1c\x1d\x1e\x1f\x7f\x85\xa0"sv)
    EXPECT_FALSE(text::ascii_space(other));
}

TEST(AsciiTrim, StripsVerticalTabAndFormFeedButNotNul) {
  EXPECT_EQ(text::trim(" \t\v\fvalue\r\n\f\v"), "value");
  EXPECT_EQ(text::trim("\0value\0"sv), "\0value\0"sv);
  EXPECT_EQ(text::trim(" \t\v "), "");
  EXPECT_EQ(text::trim("a b"), "a b");
}

TEST(AsciiCaseCompare, OrdersLikeStrcasecmp) {
  EXPECT_TRUE(std::is_eq(text::casecmp("apple", "APPLE")));
  EXPECT_TRUE(std::is_lt(text::casecmp("apple", "Banana")));
  EXPECT_TRUE(std::is_gt(text::casecmp("b", "A")));
  EXPECT_TRUE(std::is_gt(text::casecmp("ab", "a")));
  EXPECT_TRUE(std::is_lt(text::casecmp("_", "a")));
}

TEST(AsciiCaseCompare, ComparesEmbeddedNulsAsBytes) {
  EXPECT_TRUE(std::is_eq(text::casecmp("A\0b"sv, "a\0B"sv)));
  EXPECT_TRUE(std::is_lt(text::casecmp("a\0b"sv, "a\0c"sv)));
  EXPECT_TRUE(std::is_gt(text::casecmp("a\0"sv, "a"sv)));
  EXPECT_TRUE(std::is_lt(text::casecmp("a\0"sv, "a\x01"sv)));
  EXPECT_TRUE(text::ieq("Host\0X"sv, "hOST\0x"sv));
  EXPECT_FALSE(text::ieq("Host\0X"sv, "Host"sv));
}

TEST(AsciiCaseCompare, FoldsOnlyAsciiLetters) {
  EXPECT_FALSE(text::ieq("\xc3\xa9", "\xc3\x89"));
  EXPECT_FALSE(text::ieq("[", "{"));
  EXPECT_TRUE(std::is_gt(text::casecmp("\x80", "z")));
}

TEST(AsciiTruncate, KeepsWhatFitsBesideATerminator) {
  EXPECT_EQ(text::truncate("abcdef", 4), "abc");
  EXPECT_EQ(text::truncate("abc", 4), "abc");
  EXPECT_EQ(text::truncate("abc", 1), "");
  EXPECT_EQ(text::truncate("abc", 0), "");
}

TEST(AsciiTruncate, KeepsWholeUtf8Characters) {
  EXPECT_EQ(text::truncate("ab\xc3\xa9", 4), "ab");
  EXPECT_EQ(text::truncate("ab\xc3\xa9", 5), "ab\xc3\xa9");
  EXPECT_EQ(text::truncate("\xe2\x82\xac\xe2\x82\xac", 6), "\xe2\x82\xac");
  EXPECT_EQ(text::truncate("\xf0\x9f\x98\x80", 4), "");
}

TEST(AsciiClasses, SortLettersByCase) {
  for (int value = 0; value <= 0xff; ++value) {
    const auto byte = static_cast<char>(value);
    const bool lower = value >= 'a' && value <= 'z';
    const bool upper = value >= 'A' && value <= 'Z';
    EXPECT_EQ(text::ascii_lower_letter(byte), lower) << value;
    EXPECT_EQ(text::ascii_upper_letter(byte), upper) << value;
    EXPECT_EQ(text::ascii_alpha(byte), lower || upper) << value;
    EXPECT_EQ(text::json_space(byte),
              value == ' ' || value == '\t' || value == '\n' || value == '\r')
        << value;
    EXPECT_EQ(text::http_space(byte), value == ' ' || value == '\t') << value;
  }
}

TEST(AsciiTrim, TrimsOnlySpacesAndTabsAsHttpWhiteSpace) {
  EXPECT_EQ(text::trim_http_space(" \tvalue \t"), "value");
  EXPECT_EQ(text::trim_http_space("\vvalue\f"), "\vvalue\f");
  EXPECT_EQ(text::trim_http_space(" \t "), "");
}

static_assert(text::ascii_digit('7') && text::ascii_alnum('z') &&
              text::ascii_space('\v') && !text::json_space('\v') &&
              text::ascii_lower('Q') == 'q');

TEST(AsciiCString, StopsAtTheFirstNul) {
  EXPECT_EQ(text::c_string("ab\0cd\0"sv), "ab");
  EXPECT_EQ(text::c_string("abc"), "abc");
  EXPECT_EQ(text::c_string("\0abc"sv), "");
}

}  // namespace
}  // namespace text_test
