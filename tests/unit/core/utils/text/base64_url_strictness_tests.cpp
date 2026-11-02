#include "src/core/utils/text/base64.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace text_test {
namespace {

using libchiaking::text::base64::Alphabet;
using libchiaking::text::base64::decode_url;
using libchiaking::text::base64::decode_url_exact;
using libchiaking::text::base64::encode;
using test_support::Bytes;

TEST(Base64Url, EncodesWithoutPadding) {
  EXPECT_TRUE(encode(Bytes::unhex(""), Alphabet::url).empty());
  EXPECT_EQ(encode(Bytes::unhex("00"), Alphabet::url), "AA");
  EXPECT_EQ(encode(Bytes::unhex("fbff"), Alphabet::url), "-_8");
  EXPECT_EQ(encode(Bytes::unhex("000102"), Alphabet::url), "AAEC");
}

TEST(Base64Url, DecodesWhatItEncodes) {
  const auto zero = decode_url("AA");
  ASSERT_TRUE(zero);
  EXPECT_EQ(Bytes::hex(zero.value()), "00");
  const auto url = decode_url("-_8");
  ASSERT_TRUE(url);
  EXPECT_EQ(Bytes::hex(url.value()), "fbff");
  const auto three = decode_url("AAEC");
  ASSERT_TRUE(three);
  EXPECT_EQ(Bytes::hex(three.value()), "000102");
  const auto empty = decode_url("");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty.value().empty());
}

TEST(Base64Url, RejectsPaddingStrayBitsAndForeignCharacters) {
  EXPECT_FALSE(decode_url("A"));
  EXPECT_FALSE(decode_url("AA=="));
  EXPECT_FALSE(decode_url("AB"));
  EXPECT_FALSE(decode_url("-_9"));
  EXPECT_FALSE(decode_url("AA+A"));
  EXPECT_FALSE(decode_url("AA/A"));
  EXPECT_FALSE(decode_url("AA A"));
}

TEST(Base64Url, DecodesExactlyTheExpectedLength) {
  EXPECT_TRUE(decode_url_exact<2>("-_8"));
  EXPECT_FALSE(decode_url_exact<3>("-_8"));
  EXPECT_FALSE(decode_url_exact<2>("AAEC"));
}

TEST(Base64Url, LeavesNothingDecodedAfterAFailure) {
  std::array<std::byte, 3> output{};
  output.fill(std::byte{0x5a});
  EXPECT_FALSE(libchiaking::text::base64::decode_url_into("AQI!", output));
  EXPECT_EQ(output, (std::array<std::byte, 3>{}));
  output.fill(std::byte{0x5a});
  EXPECT_FALSE(libchiaking::text::base64::decode_url_into("AQ", output));
  EXPECT_EQ(output, (std::array<std::byte, 3>{}));
}

}  // namespace
}  // namespace text_test
