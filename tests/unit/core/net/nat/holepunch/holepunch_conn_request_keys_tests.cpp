#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/nat/holepunch/holepunch_conn_request_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;
namespace base64 = libchiaking::text::base64;

class HolepunchHashedId : public ConnRequestRig {
 protected:
  static auto from_text(std::string_view text) {
    Wire object{};
    object.local_hashed_id = std::string{text};
    return ConnRequestCodec::decode_hashed_id(object);
  }

  static auto field(std::string_view json)
      -> decltype(ConnRequestCodec::decode_hashed_id(std::declval<Wire>())) {
    auto parsed = read(json);
    if (!parsed) return {};
    return ConnRequestCodec::decode_hashed_id(*parsed);
  }
};

TEST_F(HolepunchHashedId, DecodesTheFieldOfARequest) {
  auto decoded = field(R"({"localHashedId":"AAECAwQFBgcICQoLDA0ODxAREhM="})");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(counts_up(decoded.value(), 20, 20));
}

TEST_F(HolepunchHashedId, ReadsAnEmptyFieldAsZeros) {
  auto decoded = field(R"({"localHashedId":""})");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(counts_up(decoded.value(), 20, 0));
}

TEST_F(HolepunchHashedId, RefusesAMissingOrMistypedField) {
  for (const auto* json :
       {R"({})", R"([])", R"({"localHashedId":null})",
        R"({"localHashedId":42})", R"({"localHashedId":true})",
        R"({"localHashedId":[]})",
        R"({"localHashedId":"-AAAAAAAAAAAAAAAAAAAAAAAAAA="})"}) {
    EXPECT_FALSE(field(json).has_value()) << json;
  }
}

TEST_F(HolepunchHashedId, DecodesStandardBase64) {
  const auto zeros = from_text("AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
  ASSERT_TRUE(zeros);
  EXPECT_TRUE(counts_up(zeros.value(), 20, 0));
  const auto sequence = from_text("AAECAwQFBgcICQoLDA0ODxAREhM=");
  ASSERT_TRUE(sequence);
  EXPECT_TRUE(counts_up(sequence.value(), 20, 20));
}

TEST_F(HolepunchHashedId, ToleratesLooseBase64AndZeroFillsShortText) {
  for (const auto text :
       {std::string_view{""}, std::string_view{"AAECAwQFBgcICQoLDA0ODxAREhM"},
        std::string_view{"AAECAwQFBgcICQoLDA0ODxAREhN="},
        std::string_view{"AAECAwQFBgcI\nCQoLDA0ODxAREhM="},
        std::string_view{"AAECAwQF"}}) {
    SCOPED_TRACE(text);
    auto result = from_text(text);
    ASSERT_TRUE(result);
    const std::size_t decoded = text.empty() ? 0 : text.size() == 8 ? 6 : 20;
    EXPECT_TRUE(counts_up(result.value(), 20, decoded));
  }
}

TEST_F(HolepunchHashedId, RefusesTextThatIsNotBase64OfTwentyBytes) {
  EXPECT_FALSE(from_text("-AAAAAAAAAAAAAAAAAAAAAAAAAA="));
  EXPECT_FALSE(from_text("AAECAwQFBgcI\nCQoLDA0ODxAREhM==="));
  EXPECT_FALSE(from_text("AAA AAAAAAAAAAAAAAAAAAAAAAA="));
  EXPECT_FALSE(
      from_text(std::string_view{"AAAAA\0AAAAAAAAAAAAAAAAAAAAA=", 28}));
  EXPECT_FALSE(from_text("AAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
}

class HolepunchSessionKey : public ConnRequestRig {
 protected:
  static auto from_text(std::string_view text) {
    Wire object{};
    object.skey = libchiaking::SecretString{text};
    return ConnRequestCodec::decode_skey(object);
  }

  static auto field(std::string_view json)
      -> decltype(ConnRequestCodec::decode_skey(std::declval<Wire>())) {
    auto parsed = read(json);
    if (!parsed) return {};
    return ConnRequestCodec::decode_skey(*parsed);
  }

  static auto accepts(std::string_view text, std::size_t decoded)
      -> ::testing::AssertionResult {
    auto result = from_text(text);
    if (!result) return ::testing::AssertionFailure() << "refused " << text;
    return counts_up(result.value(), 16, decoded);
  }
};

TEST_F(HolepunchSessionKey, DecodesTheFieldOfARequest) {
  auto decoded = field(R"({"skey":"AAECAwQFBgcICQoLDA0ODw=="})");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(counts_up(decoded.value(), 16, 16));
}

TEST_F(HolepunchSessionKey, RefusesAMissingOrMistypedField) {
  for (const auto* json :
       {R"({})", R"([])", R"({"skey":null})", R"({"skey":42})",
        R"({"skey":true})", R"({"skey":[]})",
        R"({"skey":"-AAAAAAAAAAAAAAAAAAAAA=="})"}) {
    EXPECT_FALSE(field(json).has_value()) << json;
  }
}

TEST_F(HolepunchSessionKey, AcceptsAnEmptyField) {
  EXPECT_TRUE(field(R"({"skey":""})").has_value());
}

TEST_F(HolepunchSessionKey, DecodesStandardBase64) {
  const auto zeros = from_text("AAAAAAAAAAAAAAAAAAAAAA==");
  ASSERT_TRUE(zeros);
  EXPECT_TRUE(counts_up(zeros.value(), 16, 0));
  const auto sequence = from_text("AAECAwQFBgcICQoLDA0ODw==");
  ASSERT_TRUE(sequence);
  EXPECT_TRUE(counts_up(sequence.value(), 16, 16));
}

TEST_F(HolepunchSessionKey, ToleratesLooseBase64AndZeroFillsShortText) {
  EXPECT_TRUE(accepts("", 0));
  EXPECT_TRUE(accepts("AAECAwQFBgcICQoLDA0ODw", 16));
  EXPECT_TRUE(accepts("AAECAwQF\nBgcICQoLDA0ODw==", 16));
  EXPECT_TRUE(accepts("AAECAwQFBgcICQoLDA0OD3==", 16));
  EXPECT_TRUE(accepts("AAEC", 3));
}

TEST_F(HolepunchSessionKey, RefusesTextThatIsNotBase64OfSixteenBytes) {
  EXPECT_FALSE(from_text("AAECAwQF\nBgcICQoLDA0ODw==="));
  EXPECT_FALSE(from_text("AAECAwQF=BgcICQ"));
  EXPECT_FALSE(from_text("-AAAAAAAAAAAAAAAAAAAAA=="));
  EXPECT_FALSE(from_text("AAA AAAAAAAAAAAAAAAAAA=="));
  EXPECT_FALSE(from_text(std::string_view{"AAAAA\0AAAAAAAAAAAAAAAA==", 24}));
  EXPECT_FALSE(from_text("AAAAAAAAAAAAAAAAAAAAAAA="));
  EXPECT_FALSE(from_text("AAAAAAAAAAAAAAAAAAAAAAAA"));
}

class HolepunchSecretEncoding : public ConnRequestRig {
 protected:
  template <std::size_t Size>
  static auto encode(const std::array<std::byte, Size>& bytes)
      -> libchiaking::SecretString {
    return base64::encode<libchiaking::SecretString>(
        std::span<const std::byte, Size>{bytes}, base64::Alphabet::standard);
  }
};

TEST_F(HolepunchSecretEncoding, WritesASessionKeyThatDecodesBack) {
  std::array<std::byte, 16> key{};
  EXPECT_EQ(encode(key).view(), "AAAAAAAAAAAAAAAAAAAAAA==");
  for (std::size_t index = 0; index < key.size(); ++index)
    key[index] = static_cast<std::byte>(index);
  auto key_encoded = encode(key);
  EXPECT_EQ(key_encoded.view(), "AAECAwQFBgcICQoLDA0ODw==");
  Wire key_object{};
  key_object.skey = std::move(key_encoded);
  auto key_decoded = ConnRequestCodec::decode_skey(key_object);
  ASSERT_TRUE(key_decoded.has_value());
  for (std::size_t index = 0; index < key.size(); ++index)
    EXPECT_EQ(key_decoded.value()[index], key[index]) << index;
  key.fill(std::byte{});
  key[15] = std::byte{0xff};
  EXPECT_EQ(encode(key).view().substr(20), "/w==");
}

TEST_F(HolepunchSecretEncoding, WritesAHashedIdThatDecodesBack) {
  std::array<std::byte, 20> hashed{};
  EXPECT_EQ(encode(hashed).view(), "AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
  for (std::size_t index = 0; index < hashed.size(); ++index)
    hashed[index] = static_cast<std::byte>(index);
  auto hashed_encoded = encode(hashed);
  EXPECT_EQ(hashed_encoded.view(), "AAECAwQFBgcICQoLDA0ODxAREhM=");
  Wire hashed_object{};
  hashed_object.local_hashed_id = std::string{hashed_encoded.view()};
  auto hashed_decoded = ConnRequestCodec::decode_hashed_id(hashed_object);
  ASSERT_TRUE(hashed_decoded.has_value());
  for (std::size_t index = 0; index < hashed.size(); ++index)
    EXPECT_EQ(hashed_decoded.value()[index], hashed[index]) << index;
  hashed.fill(std::byte{});
  hashed[18] = std::byte{0xff};
  hashed[19] = std::byte{0xff};
  EXPECT_EQ(encode(hashed).view().substr(24), "//8=");
}

}  // namespace
}  // namespace nat_test
