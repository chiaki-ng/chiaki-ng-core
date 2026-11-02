#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::play::InternetStart;

class PsnStartSecrets : public ::testing::Test {
 protected:
  class ScriptedRandom final : public platform::SecureRandomProvider {
   public:
    ScriptedRandom() = default;
    explicit ScriptedRandom(std::size_t fail_at) noexcept : fail_at_(fail_at) {}

    auto fill_bytes(std::span<std::byte> bytes)
        -> libchiaking::Expected<void, platform::PlatformError> override {
      lengths_.push_back(bytes.size());
      if (lengths_.size() == fail_at_) {
        std::ranges::fill(bytes, std::byte{0x7f});
        return libchiaking::unexpected(platform::PlatformError::io_failure);
      }
      const auto base = static_cast<std::uint8_t>(lengths_.size() * 0x10U);
      for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = std::byte{static_cast<std::uint8_t>(base + index)};
      return {};
    }

    auto lengths() const -> const std::vector<std::size_t>& { return lengths_; }

   private:
    std::vector<std::size_t> lengths_;
    std::size_t fail_at_{};
  };
};

TEST_F(PsnStartSecrets, DrawsTheSidHashedIdAndBothDataBlobsInOrder) {
  ScriptedRandom random;
  auto material = InternetStart::generate_secrets(random);
  ASSERT_TRUE(material);
  EXPECT_EQ(random.lengths(), (std::vector<std::size_t>{4, 20, 16, 16}));
  EXPECT_EQ(material.value().local_sid, 0x1110U);
  EXPECT_EQ(material.value().local_hashed_id[0], std::byte{0x20});
  EXPECT_EQ(material.value().local_hashed_id[19], std::byte{0x33});
  EXPECT_EQ(material.value().data1[0], std::byte{0x30});
  EXPECT_EQ(material.value().data2[15], std::byte{0x4f});
}

TEST_F(PsnStartSecrets, FailsAtWhicheverDrawTheRandomProviderRefuses) {
  for (std::size_t failure = 1; failure <= 4; ++failure) {
    SCOPED_TRACE(failure);
    ScriptedRandom random{failure};
    auto material = InternetStart::generate_secrets(random);
    ASSERT_FALSE(material);
    EXPECT_EQ(material.error(), InternetStart::SecretsError::random_failure);
    EXPECT_EQ(random.lengths().size(), failure);
  }
}

TEST_F(PsnStartSecrets, AcceptsAZeroSid) {
  test_support::FixedRandom zero_random{std::byte{}};
  auto zero_sid = InternetStart::generate_secrets(zero_random);
  ASSERT_TRUE(zero_sid);
  EXPECT_EQ(zero_sid.value().local_sid, 0);
}

}  // namespace
}  // namespace play_test
