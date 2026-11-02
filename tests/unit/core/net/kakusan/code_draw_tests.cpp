#include "src/core/net/kakusan/pake.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/runtime/random_provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::Code;
using libchiaking::net::kakusan::Exchange;

class CodeDraw : public ::testing::Test {
 protected:
  class Scripted final : public libchiaking::platform::SecureRandomProvider {
   public:
    auto fill_bytes(std::span<std::byte> output) -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      for (auto& byte : output)
        byte = std::byte{next < bytes.size() ? bytes[next++] : std::uint8_t{0}};
      return {};
    }

    std::vector<std::uint8_t> bytes;
    std::size_t next{};
  };

  class BrokenRandom final
      : public libchiaking::platform::SecureRandomProvider {
   public:
    void recover() noexcept { broken_ = false; }

    auto fill_bytes(std::span<std::byte> output) -> libchiaking::Expected<
        void, libchiaking::platform::PlatformError> override {
      std::ranges::fill(output, std::byte{0x17});
      if (broken_)
        return libchiaking::Unexpected{libchiaking::platform::PlatformError{}};
      return {};
    }

   private:
    bool broken_{true};
  };
};

TEST_F(CodeDraw, RejectsDrawsAboveTheLargestUniformMultiple) {
  Scripted entropy;
  entropy.bytes = {0xff, 0xff, 0xea, 0x60, 0xea, 0x5f, 0x27, 0x10, 0x00, 0x07};
  const auto first = Code::draw(entropy);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->view(), "9999");
  const auto second = Code::draw(entropy);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->view(), "0000");
  const auto third = Code::draw(entropy);
  ASSERT_TRUE(third);
  EXPECT_EQ(third->view(), "0007");
}

TEST_F(CodeDraw, GivesUpWhenEveryDrawIsRejected) {
  Scripted stuck;
  stuck.bytes.assign(64, 0xff);
  EXPECT_FALSE(Code::draw(stuck));
}

TEST_F(CodeDraw, FailsWhileTheRandomSourceFailsAndRecoversWithIt) {
  BrokenRandom broken;
  std::array<std::byte, 4> output{std::byte{1}, std::byte{2}, std::byte{3},
                                  std::byte{4}};
  EXPECT_FALSE(broken.fill_bytes(output));
  EXPECT_FALSE(Code::draw(broken));
  EXPECT_FALSE(Exchange::ephemeral(broken));
  broken.recover();
  EXPECT_TRUE(broken.fill_bytes(output));
  EXPECT_TRUE(std::ranges::all_of(
      output, [](std::byte byte) { return byte == std::byte{0x17}; }));
  EXPECT_TRUE(Code::draw(broken));
  EXPECT_TRUE(Exchange::ephemeral(broken));
}

}  // namespace
}  // namespace net_kakusan_test
