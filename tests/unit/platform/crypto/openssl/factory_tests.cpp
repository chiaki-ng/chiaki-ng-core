#include <libchiaking/platform/crypto/conformance.hpp>
#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/factory.hpp>

#include <gtest/gtest.h>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::DefaultPlatformFactory;
using libchiaking::platform::PlatformCapability;
using libchiaking::platform::PlatformFactory;
using libchiaking::platform::openssl::OpenSslCryptoProvider;

class OpenSslFactory : public ::testing::Test {
 protected:
  [[nodiscard]] static auto offers_crypto(const PlatformFactory& factory)
      -> bool {
    return (factory.info().capabilities & PlatformCapability::crypto) ==
           PlatformCapability::crypto;
  }

  static constexpr int iterations = 50;
};

TEST_F(OpenSslFactory, PlainDefaultFactoryOffersNoCrypto) {
  DefaultPlatformFactory factory;
  EXPECT_FALSE(factory.supplies(PlatformCapability::crypto));
  EXPECT_FALSE(offers_crypto(factory));
}

TEST_F(OpenSslFactory,
       FactoryGivenTheProviderAdvertisesItAndPassesConformance) {
  for (int iteration = 0; iteration < iterations; ++iteration) {
    OpenSslCryptoProvider provider;
    DefaultPlatformFactory factory{&provider};
    EXPECT_TRUE(offers_crypto(factory)) << "iteration " << iteration;
    PlatformFactory& base = factory;
    EXPECT_EQ(&base.crypto(), &provider) << "iteration " << iteration;
    const auto report = libchiaking::platform::crypto_conformance::run(factory);
    EXPECT_TRUE(report.ok) << "iteration " << iteration << " failed mask 0x"
                           << std::hex << report.failed_mask;
  }
}

TEST_F(OpenSslFactory, FactoryGivenANullProviderOffersNoCrypto) {
  OpenSslCryptoProvider provider;
  DefaultPlatformFactory factory{nullptr};
  EXPECT_FALSE(factory.supplies(PlatformCapability::crypto));
  EXPECT_FALSE(offers_crypto(factory));
}

}  // namespace
}  // namespace platform_crypto_test
