#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

class CauchyFec8Recovery : public ::testing::Test {
 protected:
  using Slots = std::vector<std::optional<std::vector<std::byte>>>;
  using Recovered = decltype(remote_play::CauchyFec8Decoder{}.recover(
      Slots{}, std::size_t{}, std::size_t{}));

  static auto bytes(std::initializer_list<int> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const auto value : values)
      out.push_back(static_cast<std::byte>(value));
    return out;
  }

  static auto error_of(const Recovered& recovered)
      -> std::optional<remote_play::CauchyFec8Error> {
    if (recovered) return std::nullopt;
    return recovered.error();
  }

  auto two_sources(const Recovered& recovered) const
      -> testing::AssertionResult {
    if (!recovered) return testing::AssertionFailure() << "recovery failed";
    if (recovered.value().size() != 2)
      return testing::AssertionFailure()
             << recovered.value().size() << " sources, not 2";
    if (recovered.value()[0] != source0)
      return testing::AssertionFailure() << "source 0 differs";
    if (recovered.value()[1] != source1)
      return testing::AssertionFailure() << "source 1 differs";
    return testing::AssertionSuccess();
  }

  const std::vector<std::byte> source0 = bytes({1, 2, 3, 4});
  const std::vector<std::byte> source1 = bytes({5, 6, 7, 8});
  const std::vector<std::byte> parity1 = bytes({0x8d, 0x01, 0x8e, 0x00});
  const std::vector<std::byte> parity2a = bytes({0x8d, 0x03, 0x79, 0xf1});
  const std::vector<std::byte> parity2b = bytes({0x78, 0xf6, 0x8c, 0xf3});
};

TEST_F(CauchyFec8Recovery, RecoversOneMissingUnitFromOneParity) {
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{std::nullopt, source1, parity1}, 2, 1)));
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{source0, std::nullopt, parity1}, 2, 1)));
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{source0, source1, std::nullopt}, 2, 1)));
}

TEST_F(CauchyFec8Recovery, RecoversTwoMissingUnitsFromTwoParities) {
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{std::nullopt, source1, parity2a, std::nullopt}, 2, 2)));
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{source0, std::nullopt, std::nullopt, parity2b}, 2, 2)));
  EXPECT_TRUE(two_sources(remote_play::CauchyFec8Decoder{}.recover(
      Slots{std::nullopt, std::nullopt, parity2a, parity2b}, 2, 2)));
}

TEST_F(CauchyFec8Recovery, NeedsAsManyUnitsAsSources) {
  EXPECT_EQ(
      error_of(remote_play::CauchyFec8Decoder{}.recover(
          Slots{std::nullopt, std::nullopt, parity2a, std::nullopt}, 2, 2)),
      remote_play::CauchyFec8Error::insufficient_units);
}

TEST_F(CauchyFec8Recovery, RefusesInvalidCounts) {
  EXPECT_EQ(error_of(remote_play::CauchyFec8Decoder{}.recover(Slots{}, 0, 1)),
            remote_play::CauchyFec8Error::invalid_counts);
  EXPECT_EQ(
      error_of(remote_play::CauchyFec8Decoder{}.recover(Slots{}, 200, 100)),
      remote_play::CauchyFec8Error::invalid_counts);
}

TEST_F(CauchyFec8Recovery, RefusesUnitsOfTheWrongSize) {
  EXPECT_EQ(error_of(remote_play::CauchyFec8Decoder{}.recover(
                Slots{source0, source1}, 2, 1)),
            remote_play::CauchyFec8Error::invalid_size);
  EXPECT_EQ(error_of(remote_play::CauchyFec8Decoder{}.recover(
                Slots{source0, bytes({5, 6}), parity1}, 2, 1)),
            remote_play::CauchyFec8Error::invalid_size);
  EXPECT_EQ(error_of(remote_play::CauchyFec8Decoder{}.recover(
                Slots{source0, source1, bytes({})}, 2, 1)),
            remote_play::CauchyFec8Error::invalid_size);
}

class CauchyFec8LargeRecovery : public ::testing::Test {
 protected:
  using Slots = std::vector<std::optional<std::vector<std::byte>>>;
  static constexpr std::size_t k = 150;
  static constexpr std::size_t m = 8;
  static constexpr std::size_t unit_size = 1001;

  static auto multiply(std::uint8_t a, std::uint8_t b) -> std::uint8_t {
    std::uint8_t product = 0;
    while (b != 0) {
      if ((b & 1U) != 0) product ^= a;
      const bool overflow = (a & 0x80U) != 0;
      a = static_cast<std::uint8_t>(a << 1U);
      if (overflow) a ^= 0x1dU;
      b = static_cast<std::uint8_t>(b >> 1U);
    }
    return product;
  }

  static auto inverse(std::uint8_t value) -> std::uint8_t {
    std::uint8_t result = 1;
    for (unsigned exponent = 254; exponent != 0; exponent >>= 1U) {
      if ((exponent & 1U) != 0) result = multiply(result, value);
      value = multiply(value, value);
    }
    return result;
  }

  static auto pattern(std::size_t size, std::size_t seed)
      -> std::vector<std::byte> {
    std::vector<std::byte> out(size);
    auto state = static_cast<std::uint32_t>(seed) * 2654435761U + 1U;
    for (auto& value : out) {
      state = state * 1664525U + 1013904223U;
      value = static_cast<std::byte>(state >> 24U);
    }
    return out;
  }

  void SetUp() override {
    for (std::size_t source = 0; source < k; ++source)
      sources.push_back(pattern(unit_size, source));
    slots.assign(sources.begin(), sources.end());
    for (std::size_t row = 0; row < m; ++row) {
      std::vector<std::byte> parity(unit_size);
      for (std::size_t source = 0; source < k; ++source) {
        const auto factor =
            inverse(static_cast<std::uint8_t>(row ^ (m + source)));
        for (std::size_t offset = 0; offset < unit_size; ++offset)
          parity[offset] ^= static_cast<std::byte>(multiply(
              factor, std::to_integer<std::uint8_t>(sources[source][offset])));
      }
      slots.emplace_back(std::move(parity));
    }
  }

  auto recovers_without(std::initializer_list<std::size_t> lost)
      -> testing::AssertionResult {
    auto damaged = slots;
    for (const auto index : lost) damaged[index].reset();
    const auto result = decoder.recover(std::move(damaged), k, m);
    if (!result) return testing::AssertionFailure() << "recovery failed";
    if (result.value() != sources)
      return testing::AssertionFailure() << "the recovered sources differ";
    return testing::AssertionSuccess();
  }

  std::vector<std::vector<std::byte>> sources;
  Slots slots;
  remote_play::CauchyFec8Decoder decoder;
};

TEST_F(CauchyFec8LargeRecovery, RecoversAnyCombinationOfUpToEightLostUnits) {
  EXPECT_TRUE(recovers_without({}));
  EXPECT_TRUE(recovers_without({0}));
  EXPECT_TRUE(recovers_without({3, 77, 149}));
  EXPECT_TRUE(recovers_without({1, 2, 3, 4, 5, k + 1, k + 6}));
  EXPECT_TRUE(recovers_without({10, 20, 30, 40, 50, 60, 70, 80}));
  EXPECT_TRUE(recovers_without({k, k + 2, 9, 99}));
}

TEST_F(CauchyFec8LargeRecovery, RefusesNineLostUnits) {
  auto too_many = slots;
  for (std::size_t index = 0; index < m + 1; ++index) too_many[index].reset();
  const auto result = decoder.recover(std::move(too_many), k, m);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::CauchyFec8Error::insufficient_units);
}

}  // namespace
}  // namespace remote_play_test
