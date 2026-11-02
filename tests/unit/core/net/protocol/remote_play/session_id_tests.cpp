#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

static_assert(!std::is_copy_constructible_v<remote_play::SessionId>);
static_assert(std::is_nothrow_move_constructible_v<remote_play::SessionId>);

class SessionIdParse : public ::testing::Test {
 protected:
  static auto payload(std::string_view id, std::byte marker = std::byte{0x4a})
      -> std::vector<std::byte> {
    std::vector<std::byte> wire{marker};
    for (char c : id) wire.push_back(static_cast<std::byte>(c));
    return wire;
  }
};

TEST_F(SessionIdParse, AcceptsTheShortestIdWithTheExpectedMarker) {
  const std::string short_id(24, 'A');
  const auto min = remote_play::SessionId::parse(payload(short_id));
  ASSERT_TRUE(min);
  EXPECT_EQ(min.value().view(), short_id);
  EXPECT_FALSE(min.value().unexpected_marker());
}

TEST_F(SessionIdParse, AcceptsTheLongestIdAndFlagsAnUnexpectedMarker) {
  const std::string long_id(78, '9');
  const auto max =
      remote_play::SessionId::parse(payload(long_id, std::byte{0}));
  ASSERT_TRUE(max);
  EXPECT_EQ(max.value().view(), long_id);
  EXPECT_TRUE(max.value().unexpected_marker());
}

TEST_F(SessionIdParse, KeepsItsOwnCopyOfMixedCaseText) {
  auto mixed = payload("ABCdef0123456789ABCdef01");
  const auto parsed = remote_play::SessionId::parse(mixed);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().view(), "ABCdef0123456789ABCdef01");
  mixed[1] = std::byte{'Z'};
  EXPECT_EQ(parsed.value().view(), "ABCdef0123456789ABCdef01");
}

TEST_F(SessionIdParse, RefusesAnEmptyPayloadAsTooShort) {
  const auto empty = remote_play::SessionId::parse({});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), remote_play::SessionIdError::too_short);
}

TEST_F(SessionIdParse, RefusesTwentyThreeCharactersAsTooShort) {
  const auto short_id =
      remote_play::SessionId::parse(payload(std::string(23, 'a')));
  ASSERT_FALSE(short_id);
  EXPECT_EQ(short_id.error(), remote_play::SessionIdError::too_short);
}

TEST_F(SessionIdParse, RefusesSeventyNineCharactersAsTooLong) {
  const auto long_id =
      remote_play::SessionId::parse(payload(std::string(79, 'a')));
  ASSERT_FALSE(long_id);
  EXPECT_EQ(long_id.error(), remote_play::SessionIdError::too_long);
}

TEST_F(SessionIdParse, RefusesPunctuationAndNulCharacters) {
  auto bad = payload(std::string(24, 'a'));
  bad[12] = std::byte{'-'};
  const auto invalid = remote_play::SessionId::parse(bad);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), remote_play::SessionIdError::invalid_character);
  bad[12] = std::byte{0};
  const auto nul = remote_play::SessionId::parse(bad);
  ASSERT_FALSE(nul);
  EXPECT_EQ(nul.error(), remote_play::SessionIdError::invalid_character);
}

class SessionIdFallback : public ::testing::Test {
 protected:
  std::vector<std::byte> random = std::vector<std::byte>(48, std::byte{0});
};

TEST_F(SessionIdFallback, PrefixesTheTimestampToTheEncodedRandom) {
  const auto zero = remote_play::SessionId::fallback(0, random);
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero.value().view(), "0" + std::string(64, 'A'));
  const auto boundary =
      remote_play::SessionId::fallback(999'999'999'999'999ULL, random);
  ASSERT_TRUE(boundary);
  EXPECT_EQ(boundary.value().view(), "999999999999999" + std::string(64, 'A'));
}

TEST_F(SessionIdFallback, EncodesRandomBytesAsBase64) {
  random[3] = std::byte{0xff};
  random[4] = std::byte{0xff};
  random[5] = std::byte{0xff};
  const auto mixed = remote_play::SessionId::fallback(1, random);
  ASSERT_TRUE(mixed);
  EXPECT_EQ(mixed.value().view(), "1AAAA////" + std::string(56, 'A'));
}

TEST_F(SessionIdFallback, RefusesRandomThatIsNotFortyEightBytes) {
  const auto short_random = remote_play::SessionId::fallback(
      1, std::span<const std::byte>(random).first(47));
  ASSERT_FALSE(short_random);
  EXPECT_EQ(short_random.error(),
            remote_play::SessionIdError::invalid_random_length);
}

}  // namespace
}  // namespace remote_play_test
