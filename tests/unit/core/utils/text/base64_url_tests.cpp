#include "src/core/utils/text/base64.hpp"
#include "tests/unit/core/utils/text/base64_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace text_test {
namespace {

using libchiaking::text::base64::Alphabet;
using libchiaking::text::base64::decode_url;
using libchiaking::text::base64::decode_url_exact;
using libchiaking::text::base64::decode_url_into;
using libchiaking::text::base64::encode;

TEST(Base64Url, MatchesTheRfc4648VectorsWithoutPadding) {
  constexpr std::array<Base64Rig::Vector, 7> vectors{{{"", ""},
                                                      {"f", "Zg"},
                                                      {"fo", "Zm8"},
                                                      {"foo", "Zm9v"},
                                                      {"foob", "Zm9vYg"},
                                                      {"fooba", "Zm9vYmE"},
                                                      {"foobar", "Zm9vYmFy"}}};
  for (const auto& vector : vectors) {
    EXPECT_EQ(encode(Base64Rig::bytes_of(vector.plain), Alphabet::url),
              vector.encoded);
    const auto decoded = decode_url(vector.encoded);
    ASSERT_TRUE(decoded) << vector.encoded;
    EXPECT_EQ(Base64Rig::text_of(*decoded), vector.plain);
  }
}

TEST(Base64Url, RefusesPaddingAndImpossibleTails) {
  EXPECT_FALSE(decode_url("Zg=="));
  EXPECT_FALSE(decode_url("Zm8="));
  EXPECT_FALSE(decode_url("Zh"));
  EXPECT_FALSE(decode_url("Zm9"));
  EXPECT_FALSE(decode_url("Z"));
  EXPECT_FALSE(decode_url("Zm9vY"));
}

TEST(Base64Url, EncodesWithTheUrlAlphabet) {
  EXPECT_EQ(encode(Base64Rig::high, Alphabet::url), "-_-_");
  EXPECT_EQ(encode(std::span{Base64Rig::high}.first(2), Alphabet::url), "-_8");
  EXPECT_EQ(encode(std::span{Base64Rig::high}.first(1), Alphabet::url), "-w");
  std::string wiped;
  libchiaking::text::base64::append(wiped, Base64Rig::high, Alphabet::url);
  EXPECT_EQ(wiped, "-_-_");
}

TEST(Base64Url, DecodesTheUrlAlphabet) {
  const auto decoded = decode_url("-_-_");
  ASSERT_TRUE(decoded);
  ASSERT_EQ(decoded->size(), 3U);
  EXPECT_EQ((*decoded)[0], Base64Rig::high[0]);
  EXPECT_EQ((*decoded)[1], Base64Rig::high[1]);
  EXPECT_EQ((*decoded)[2], Base64Rig::high[2]);
  const auto empty = decode_url("");
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->empty());
}

TEST(Base64Url, RefusesSymbolsOutsideTheUrlAlphabet) {
  EXPECT_FALSE(decode_url("+/+/"));
  EXPECT_FALSE(decode_url("AA+A"));
  EXPECT_FALSE(decode_url("AA/A"));
  EXPECT_FALSE(decode_url("AA A"));
  EXPECT_FALSE(decode_url("AA\nA"));
  EXPECT_FALSE(decode_url("AA=A"));
  EXPECT_FALSE(decode_url(std::string_view{"AA\0A", 4}));
  EXPECT_FALSE(decode_url("AB"));
  EXPECT_FALSE(decode_url("-_9"));
}

TEST(Base64Url, DecodesExactLengths) {
  EXPECT_TRUE(decode_url_exact<2>("-_8"));
  EXPECT_FALSE(decode_url_exact<3>("-_8"));
  EXPECT_FALSE(decode_url_exact<2>("AAEC"));
  EXPECT_FALSE(decode_url_exact<2>("-_9"));
  EXPECT_TRUE(decode_url_exact<0>(""));
  EXPECT_FALSE(decode_url_exact<0>("AA"));
  const auto key =
      decode_url_exact<32>("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");
  ASSERT_TRUE(key);
  for (std::size_t index = 0; index < key->size(); ++index)
    EXPECT_EQ(std::to_integer<std::size_t>((*key)[index]), index);
  EXPECT_FALSE(
      decode_url_exact<32>("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="));
  EXPECT_FALSE(
      decode_url_exact<32>("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh"));
}

TEST(Base64Url, DecodesIntoABufferThatFits) {
  std::array<std::byte, 4> small{};
  EXPECT_FALSE(decode_url_into("AAECAwQF", small));
  EXPECT_FALSE(decode_url_into("AAEC", small));
  EXPECT_TRUE(decode_url_into("AAECAw", small));
  EXPECT_EQ(small[3], std::byte{3});
}

TEST(Base64Url, RoundTripsEveryLength) {
  std::vector<std::byte> bytes;
  for (std::size_t length = 0; length <= 300; ++length) {
    const auto text = encode(bytes, Alphabet::url);
    EXPECT_EQ(text.size(), (length * 4 + 2) / 3);
    EXPECT_EQ(text.find_first_of("+/="), std::string::npos) << text;
    const auto decoded = decode_url(text);
    ASSERT_TRUE(decoded) << "length " << length;
    EXPECT_EQ(*decoded, bytes) << "length " << length;
    bytes.push_back(Base64Rig::pattern_byte(length));
  }
}

TEST(Base64Url, DecodesTheFixtureBlocks) {
  constexpr std::string_view prefix = "RCP1:";
  for (const auto* name : {"invitation", "reply"}) {
    const auto contents = Base64Rig::load(std::string{name} + ".txt");
    ASSERT_TRUE(contents) << name;
    std::string block;
    for (const auto symbol : *contents) {
      if (symbol != ' ' && symbol != '\t' && symbol != '\r' && symbol != '\n')
        block.push_back(symbol);
    }
    EXPECT_EQ(block.compare(0, prefix.size(), prefix), 0) << name;
    const auto text = std::string_view{block}.substr(prefix.size());
    const auto decoded = decode_url(text);
    ASSERT_TRUE(decoded) << name;
    ASSERT_FALSE(decoded->empty()) << name;
    EXPECT_EQ(encode(*decoded, Alphabet::url), text);
    EXPECT_TRUE(decoded->front() == std::byte{0x51} ||
                decoded->front() == std::byte{0x52})
        << name;
    EXPECT_FALSE(decode_url(std::string{text} + "=")) << name;
  }
}

}  // namespace
}  // namespace text_test
