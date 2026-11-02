#include "libchiaking/kakusan_types.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <string>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::RememberedDevices;
using std::string_literals::operator""s;

TEST(RememberedNickname, IsTrimmedAndAtMostFortyCharacters) {
  EXPECT_EQ(RememberedDevices::longest_nickname, 40);
  EXPECT_EQ(RememberedDevices::normalised("  Sam \n"), "Sam");
  EXPECT_EQ(RememberedDevices::normalised("Example Artist"), "Example Artist");
  EXPECT_FALSE(RememberedDevices::normalised(""));
  EXPECT_FALSE(RememberedDevices::normalised(" \t\n"));
  EXPECT_FALSE(RememberedDevices::normalised("Sa\tm"));
  EXPECT_FALSE(RememberedDevices::normalised(std::string{"Sa\x7fm"}));
  EXPECT_FALSE(RememberedDevices::normalised(std::string{"Sa\xc2\x85m"}));
  EXPECT_TRUE(RememberedDevices::normalised(std::string(40, 'a')));
  EXPECT_FALSE(RememberedDevices::normalised(std::string(41, 'a')));
  std::string accented;
  for (int index = 0; index < 40; ++index) accented += "\xc3\xa9";
  EXPECT_TRUE(RememberedDevices::normalised(accented));
  EXPECT_FALSE(RememberedDevices::normalised(accented + "\xc3\xa9"));
}

TEST(RememberedNickname, MatchesIgnoringCaseAndAccents) {
  EXPECT_TRUE(RememberedDevices::same_nickname("Sam", "sAM"));
  EXPECT_TRUE(RememberedDevices::same_nickname("Jos\xc3\xa9", "jose"));
  EXPECT_TRUE(RememberedDevices::same_nickname("Jose\xcc\x81", "JOSE"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x85sa", "asa"));
  EXPECT_FALSE(RememberedDevices::same_nickname("Sam", "Sam "));
  EXPECT_FALSE(RememberedDevices::same_nickname("Sam", "Pam"));
}

TEST(RememberedNickname, KeepsTheMeaningOfItsEdgeCharacters) {
  EXPECT_EQ(RememberedDevices::normalised("\v\fSam\r"), "Sam");
  EXPECT_TRUE(RememberedDevices::normalised("Sa\xc2\xa0m"s));
  EXPECT_FALSE(RememberedDevices::normalised("Sa\xc2\x9fm"s));
  EXPECT_TRUE(RememberedDevices::normalised("Sa\xffm"s));
  EXPECT_TRUE(RememberedDevices::normalised("\xe6\x97\xa5\xe6\x9c\xac"s));
  std::string faces;
  for (int count = 0; count < 40; ++count) faces += "\xf0\x9f\x98\x80";
  EXPECT_TRUE(RememberedDevices::normalised(faces));
  EXPECT_FALSE(RememberedDevices::normalised(faces + "\xf0\x9f\x98\x80"));
}

TEST(RememberedNickname, FoldsOnlyTheLettersThatHaveACase) {
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x89LAN", "\xc3\xa9lan"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x86on", "\xc3\xa6on"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\xbf", "Y"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x90", "\xc3\xb0"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x9e", "\xc3\xbe"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x98", "\xc3\xb8"));
  EXPECT_FALSE(RememberedDevices::same_nickname("\xc3\xb7", "\xc3\xb8"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x97", "\xc3\xb7"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xc3\x9f", "\xc3\x9f"));
  EXPECT_FALSE(RememberedDevices::same_nickname("\xc3\x9f", "ss"));
  EXPECT_TRUE(RememberedDevices::same_nickname("a\xcd\xaf", "A"));
  EXPECT_FALSE(RememberedDevices::same_nickname("a\xcd\xb0", "A"));
  EXPECT_TRUE(RememberedDevices::same_nickname("\xe6\x97\xa5", "\xe6\x97\xa5"));
  EXPECT_FALSE(
      RememberedDevices::same_nickname("\xe6\x97\xa5", "\xe6\x97\xa6"));
  EXPECT_TRUE(
      RememberedDevices::same_nickname("\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"));
  EXPECT_FALSE(
      RememberedDevices::same_nickname("\xf0\x9f\x98\x80", "\xf0\x9f\x98\x81"));
  EXPECT_FALSE(RememberedDevices::same_nickname("\xd0\x90", "\xd0\xb0"));
}

}  // namespace
}  // namespace kakusan_test
