#include <libchiaking/platform/curl/services.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace platform_curl_test {
namespace {

using namespace libchiaking;
using namespace libchiaking::platform;

static_assert(!std::is_copy_constructible_v<CurlPlatformServices>);
static_assert(!std::is_copy_assignable_v<CurlPlatformServices>);
static_assert(!std::is_move_constructible_v<CurlPlatformServices>);
static_assert(!std::is_move_assignable_v<CurlPlatformServices>);

class CurlServices : public ::testing::Test {
 protected:
  class FakeCryptoProvider final : public CryptoProvider {
   public:
    explicit FakeCryptoProvider(bool& destroyed) noexcept
        : destroyed_(&destroyed) {}
    ~FakeCryptoProvider() override { *destroyed_ = true; }

    auto sha256(std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, CryptoError> override {
      return {};
    }
    auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                     std::span<std::byte>)
        -> Expected<void, CryptoError> override {
      return {};
    }
    auto aes128_encrypt_block(std::span<const std::byte>,
                              std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, CryptoError> override {
      return {};
    }
    auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                     std::span<const std::byte>, std::span<std::byte>)
        -> Expected<void, CryptoError> override {
      return {};
    }

   private:
    bool* destroyed_;
  };

  class TemporaryDirectory {
   public:
    TemporaryDirectory() {
      const auto base = std::filesystem::temp_directory_path();
      std::random_device device;
      const auto seed = (static_cast<std::uint64_t>(device()) << 32U) |
                        static_cast<std::uint64_t>(device());
      for (int attempt = 0; attempt < 64; ++attempt) {
        const auto candidate =
            base / ("libchiaki-curl-services-" +
                    std::to_string(seed + static_cast<std::uint64_t>(attempt)));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
          path_ = candidate.string();
          return;
        }
      }
    }

    ~TemporaryDirectory() {
      std::error_code error;
      if (!path_.empty()) (void)std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::string& path() const noexcept { return path_; }

   private:
    std::string path_;
  };

  void SetUp() override { ASSERT_FALSE(temporary.path().empty()); }

  std::unique_ptr<CurlPlatformServices> services_with_crypto() {
    auto crypto = std::make_unique<FakeCryptoProvider>(crypto_destroyed);
    given_crypto = crypto.get();
    return std::make_unique<CurlPlatformServices>(temporary.path() + "/store",
                                                  "libchiaki", "platform",
                                                  std::move(crypto));
  }

  TemporaryDirectory temporary;
  bool crypto_destroyed = false;
  CryptoProvider* given_crypto = nullptr;
};

TEST_F(CurlServices, HandOutTheirOwnProvidersThroughTheFactory) {
  auto services = services_with_crypto();
  PlatformFactory& factory = services->factory();
  EXPECT_EQ(&factory.crypto(), given_crypto);
  EXPECT_EQ(&factory.http(), &services->http());
  EXPECT_EQ(&factory.websocket(), &services->websocket());
  EXPECT_EQ(&factory.diagnostics(), &services->diagnostics());
}

TEST_F(CurlServices, ReportEveryCapabilityTheyProvide) {
  auto services = services_with_crypto();
  const auto required =
      PlatformCapability::threading | PlatformCapability::monotonic_clock |
      PlatformCapability::secure_random | PlatformCapability::network |
      PlatformCapability::crypto | PlatformCapability::http |
      PlatformCapability::websocket | PlatformCapability::diagnostics;
  EXPECT_EQ(services->factory().info().capabilities & required, required);
}

TEST_F(CurlServices, FillRandomBytes) {
  auto services = services_with_crypto();
  std::array<std::byte, 8> random{};
  EXPECT_TRUE(services->factory().random().fill_bytes(random));
}

TEST_F(CurlServices, StoreARecordAndReadItBack) {
  auto services = services_with_crypto();
  const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xde},
                                       std::byte{0xad}, std::byte{0xff}};
  const auto record = RecordName::parse("catalog").value();
  ASSERT_TRUE(services->storage().write_atomic(record, payload));
  const auto stored = services->storage().read(record);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored.value().data, payload);
}

TEST_F(CurlServices, DestroyTheCryptoProviderTheyWereGiven) {
  auto services = services_with_crypto();
  EXPECT_FALSE(crypto_destroyed);
  services.reset();
  EXPECT_TRUE(crypto_destroyed);
}

TEST_F(CurlServices, ReportNoCryptoWhenGivenNone) {
  CurlPlatformServices without_crypto(
      temporary.path() + "/store-without-crypto", "libchiaki", "platform");
  EXPECT_EQ(without_crypto.crypto(), nullptr);
  EXPECT_FALSE(
      without_crypto.factory().supplies(PlatformCapability::crypto));
  EXPECT_EQ(
      without_crypto.factory().info().capabilities & PlatformCapability::crypto,
      PlatformCapability::none);
}

}  // namespace
}  // namespace platform_curl_test
