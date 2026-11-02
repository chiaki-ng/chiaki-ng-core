#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"

#include <galois.h>

extern "C" {
#include <gf_cpu.h>
}

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace remote_play_test {
namespace {

class CauchyFec8Field : public ::testing::Test {
 protected:
  [[nodiscard]] static auto bitwise_multiply(std::uint8_t a, std::uint8_t b)
      -> std::uint8_t {
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

  [[nodiscard]] static auto bitwise_inverse(std::uint8_t value)
      -> std::uint8_t {
    std::uint8_t result = 1;
    for (unsigned exponent = 254; exponent != 0; exponent >>= 1U) {
      if ((exponent & 1U) != 0) result = bitwise_multiply(result, value);
      value = bitwise_multiply(value, value);
    }
    return result;
  }

  [[nodiscard]] static auto pattern(std::size_t size, std::size_t seed)
      -> std::vector<std::byte> {
    std::vector<std::byte> out(size);
    auto state = static_cast<std::uint32_t>(seed) * 2654435761U + 1U;
    for (auto& value : out) {
      state = state * 1664525U + 1013904223U;
      value = static_cast<std::byte>(state >> 24U);
    }
    return out;
  }
};

TEST_F(CauchyFec8Field, PicksNeonOnAarch64) {
  EXPECT_EQ(galois_init_default_field(8), 0);
#if defined(__aarch64__)
  EXPECT_EQ(gf_cpu_supports_arm_neon, 1);
#endif
}

TEST_F(CauchyFec8Field, MultipliesLikeTheBitwiseField) {
  for (int a = 0; a < 256; ++a) {
    for (int b = 0; b < 256; ++b)
      ASSERT_EQ(galois_single_multiply(a, b, 8),
                bitwise_multiply(static_cast<std::uint8_t>(a),
                                 static_cast<std::uint8_t>(b)))
          << a << " * " << b;
    if (a != 0) {
      ASSERT_EQ(galois_inverse(a, 8),
                bitwise_inverse(static_cast<std::uint8_t>(a)))
          << "inverse of " << a;
    }
  }
}

TEST_F(CauchyFec8Field, MultipliesRegionsLikeTheBitwiseField) {
  for (int factor = 0; factor < 256; ++factor) {
    for (const std::size_t size :
         {1U, 7U, 15U, 16U, 17U, 31U, 33U, 63U, 1001U}) {
      for (const std::size_t shift : {0U, 1U, 5U, 9U}) {
        const auto source_store = pattern(
            size + shift, static_cast<std::size_t>(factor) * 131U + size);
        const auto original_store = pattern(
            size + shift, static_cast<std::size_t>(factor) * 137U + size);
        auto source_copy = source_store;
        auto added = original_store;
        auto written = original_store;
        auto* source = reinterpret_cast<char*>(source_copy.data() + shift);
        galois_w08_region_multiply(
            source, factor, static_cast<int>(size),
            reinterpret_cast<char*>(added.data() + shift), 1);
        galois_w08_region_multiply(
            source, factor, static_cast<int>(size),
            reinterpret_cast<char*>(written.data() + shift), 0);
        for (std::size_t offset = 0; offset < size; ++offset) {
          const auto product = static_cast<std::byte>(bitwise_multiply(
              static_cast<std::uint8_t>(factor),
              std::to_integer<std::uint8_t>(source_store[shift + offset])));
          ASSERT_EQ(added[shift + offset],
                    original_store[shift + offset] ^ product)
              << "factor " << factor << " size " << size << " shift " << shift
              << " offset " << offset;
          ASSERT_EQ(written[shift + offset], product)
              << "factor " << factor << " size " << size << " shift " << shift
              << " offset " << offset;
        }
      }
    }
  }
}

}  // namespace
}  // namespace remote_play_test
