#include "src/core/utils/text/base64.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/utils/text/base64_rig.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace text_test {
namespace {

using libchiaking::text::base64::Alphabet;
using libchiaking::text::base64::decode_into;
using libchiaking::text::base64::DecodeError;
using libchiaking::text::base64::encode;
using libchiaking::text::base64::encode_into;
using libchiaking::text::base64::encoded_size;

TEST(Base64Standard, MatchesTheRfc4648VectorsWithPadding) {
  constexpr std::array<Base64Rig::Vector, 7> vectors{{{"", ""},
                                                      {"f", "Zg=="},
                                                      {"fo", "Zm8="},
                                                      {"foo", "Zm9v"},
                                                      {"foob", "Zm9vYg=="},
                                                      {"fooba", "Zm9vYmE="},
                                                      {"foobar", "Zm9vYmFy"}}};
  for (const auto& vector : vectors)
    EXPECT_EQ(encode(Base64Rig::bytes_of(vector.plain), Alphabet::standard),
              vector.encoded);
  EXPECT_EQ(encode(Base64Rig::high, Alphabet::standard), "+/+/");
}

TEST(Base64Standard, AppendsWhatItEncodesInOneGo) {
  std::vector<std::byte> bytes;
  for (std::size_t length = 0; length <= 300; ++length) {
    for (const auto alphabet : {Alphabet::standard, Alphabet::url}) {
      std::string whole(encoded_size(length, alphabet), '\0');
      EXPECT_EQ(encode_into(bytes, whole, alphabet), whole.size())
          << "length " << length;
      EXPECT_EQ(encode(bytes, alphabet), whole);
      libchiaking::SecretString secret;
      libchiaking::text::base64::append(secret, bytes, alphabet);
      EXPECT_EQ(secret.view(), whole);
    }
    std::vector<std::byte> decoded(length);
    const auto size = decode_into(encode(bytes, Alphabet::standard), decoded,
                                  Alphabet::standard);
    ASSERT_TRUE(size) << "length " << length;
    EXPECT_EQ(size.value(), length);
    EXPECT_EQ(decoded, bytes) << "length " << length;
    bytes.push_back(Base64Rig::pattern_byte(length));
  }
}

TEST(Base64Standard, DecodesAsSimdutfDoes) {
  std::array<std::byte, 4> output{};
  EXPECT_EQ(decode_into("AQID", output, Alphabet::standard).value(), 3U);
  EXPECT_EQ(decode_into("AQI=", output, Alphabet::standard).value(), 2U);
  EXPECT_EQ(decode_into("AQI", output, Alphabet::standard).value(), 2U);
  EXPECT_EQ(decode_into("AQ\nID", output, Alphabet::standard).value(), 3U);
  EXPECT_EQ(decode_into(" AQID ", output, Alphabet::standard).value(), 3U);
  EXPECT_EQ(decode_into("", output, Alphabet::standard).value(), 0U);
}

TEST(Base64Standard, RefusesWhatSimdutfRefuses) {
  std::array<std::byte, 4> output{};
  for (const std::string_view bad :
       {std::string_view("AQ-_"), std::string_view("AQID=junk"),
        std::string_view("AQIDA"), std::string_view("AQ==="),
        std::string_view("AQ!D")}) {
    const auto result = decode_into(bad, output, Alphabet::standard);
    ASSERT_FALSE(result) << bad;
    EXPECT_EQ(result.error(), DecodeError::invalid) << bad;
  }
  const auto overflow = decode_into("AQIDBAU=", output, Alphabet::standard);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), DecodeError::too_long);
}

TEST(Base64Standard, EncodeIntoRefusesShortBuffers) {
  const auto bytes = Base64Rig::bytes_of("foobar");
  std::array<char, 8> exact{};
  EXPECT_EQ(encode_into(bytes, exact, Alphabet::standard), 8U);
  std::array<char, 7> short_standard{};
  EXPECT_FALSE(encode_into(bytes, short_standard, Alphabet::standard));
  const auto four = Base64Rig::bytes_of("foob");
  std::array<char, 6> url_exact{};
  EXPECT_EQ(encode_into(four, url_exact, Alphabet::url), 6U);
  std::array<char, 5> short_url{};
  EXPECT_FALSE(encode_into(four, short_url, Alphabet::url));
  std::array<char, 0> none{};
  EXPECT_EQ(encode_into({}, none, Alphabet::standard), 0U);
}

TEST(Base64Standard, LeavesNothingDecodedAfterAFailure) {
  std::array<std::byte, 4> output{};
  output.fill(std::byte{0x5a});
  EXPECT_FALSE(decode_into("AQID!", output, Alphabet::standard));
  EXPECT_EQ(output, (std::array<std::byte, 4>{}));
  output.fill(std::byte{0x5a});
  EXPECT_FALSE(decode_into("AQIDBAU=", output, Alphabet::standard));
  EXPECT_EQ(output, (std::array<std::byte, 4>{}));
}

}  // namespace
}  // namespace text_test
