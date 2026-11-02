#include "src/core/consoles/adapters/pairing/pairing_knock.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::consoles::PairingKnock;

class PairingKnockCodec : public ::testing::TestWithParam<Generation> {
 protected:
  static auto b(char c) noexcept -> std::byte {
    return static_cast<std::byte>(c);
  }

  auto own() const -> char { return GetParam() == Generation::ps4 ? '2' : '3'; }

  auto other() const -> char {
    return GetParam() == Generation::ps4 ? '3' : '2';
  }
};

TEST(PairingKnockDatagram, IsSrc2ForAPs4) {
  const auto datagram = PairingKnock::datagram(Generation::ps4);
  ASSERT_TRUE(datagram);
  EXPECT_EQ(
      datagram.value(),
      (PairingKnock::Datagram{std::byte{'S'}, std::byte{'R'}, std::byte{'C'},
                              std::byte{'2'}, std::byte{0}}));
}

TEST(PairingKnockDatagram, IsSrc3ForAPs5) {
  const auto datagram = PairingKnock::datagram(Generation::ps5);
  ASSERT_TRUE(datagram);
  EXPECT_EQ(
      datagram.value(),
      (PairingKnock::Datagram{std::byte{'S'}, std::byte{'R'}, std::byte{'C'},
                              std::byte{'3'}, std::byte{0}}));
}

TEST(PairingKnockDatagram, IsRefusedForAnUnknownGeneration) {
  EXPECT_FALSE(PairingKnock::datagram(static_cast<Generation>(99)));
}

TEST(PairingKnockAnswer, IsRefusedForAnUnknownGeneration) {
  constexpr std::array invalid_response{std::byte{'R'}, std::byte{'E'},
                                        std::byte{'S'}, std::byte{'3'}};
  EXPECT_FALSE(
      PairingKnock::answers(static_cast<Generation>(99), invalid_response));
}

TEST_P(PairingKnockCodec, AcceptsTheExactAnswer) {
  const std::array exact{b('R'), b('E'), b('S'), b(own())};
  EXPECT_TRUE(PairingKnock::answers(GetParam(), exact));
}

TEST_P(PairingKnockCodec, AcceptsANulTerminatedAnswer) {
  const std::array terminated{b('R'), b('E'), b('S'), b(own()), std::byte{0}};
  EXPECT_TRUE(PairingKnock::answers(GetParam(), terminated));
}

TEST_P(PairingKnockCodec, AcceptsAnyTrailingBytes) {
  const std::array trailing{b('R'),          b('E'), b('S'), b(own()),
                            std::byte{0xff}, b('x'), b(0)};
  EXPECT_TRUE(PairingKnock::answers(GetParam(), trailing));
}

TEST_P(PairingKnockCodec, RefusesTheOtherGenerationsAnswer) {
  const std::array cross{b('R'), b('E'), b('S'), b(other())};
  EXPECT_FALSE(PairingKnock::answers(GetParam(), cross));
}

TEST_P(PairingKnockCodec, RefusesEveryShortPrefix) {
  for (std::size_t length = 0; length < 4; ++length) {
    std::array<std::byte, 8> buffer{};
    for (std::size_t index = 0; index < length; ++index)
      buffer[index] = b("RES"[index]);
    EXPECT_FALSE(PairingKnock::answers(
        GetParam(), std::span<const std::byte>(buffer).first(length)))
        << length;
    if (length > 0) {
      buffer[length - 1] = b(own());
      EXPECT_FALSE(PairingKnock::answers(
          GetParam(), std::span<const std::byte>(buffer).first(length)))
          << length;
    }
  }
}

TEST_P(PairingKnockCodec, RefusesAnAnswerWithAnyByteChanged) {
  std::array<std::byte, 4> mutated{b('R'), b('E'), b('S'), b(own())};
  for (std::size_t index = 0; index < 4; ++index) {
    const auto original = mutated[index];
    mutated[index] = b('Q');
    EXPECT_FALSE(PairingKnock::answers(GetParam(), mutated)) << index;
    mutated[index] = original;
    EXPECT_TRUE(PairingKnock::answers(GetParam(), mutated)) << index;
  }
}

INSTANTIATE_TEST_SUITE_P(EachGeneration, PairingKnockCodec,
                         ::testing::Values(Generation::ps4, Generation::ps5));

}  // namespace
}  // namespace consoles_test
