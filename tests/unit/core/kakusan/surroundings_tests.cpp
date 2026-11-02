#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/fake_secure_datagram.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/kakusan/kakusan_core.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/fakes.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <utility>

namespace kakusan_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::kakusan::DeviceIdentity;
using libchiaking::kakusan::Kakusan;
using libchiaking::kakusan::Refusal;
using libchiaking::kakusan::Surroundings;
using libchiaking::testing::FakeSecureDatagramProvider;
using libchiaking::testing::ServiceBuilder;
using test_support::kakusan::LoopbackExchange;
using test_support::kakusan::LoopbackFactory;
using Unavailable = Kakusan::Unavailable;

class KakusanPlatform : public ::testing::Test {
 protected:
  class PrimitivesOnly final : public platform::CryptoProvider {
   public:
    [[nodiscard]] auto sha256(std::span<const std::byte> input,
                              std::span<std::byte> digest)
        -> libchiaking::Expected<void, platform::CryptoError> override {
      return full_.sha256(input, digest);
    }

    [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                   std::span<const std::byte> input,
                                   std::span<std::byte> digest)
        -> libchiaking::Expected<void, platform::CryptoError> override {
      return full_.hmac_sha256(key, input, digest);
    }

    [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                            std::span<const std::byte> block,
                                            std::span<std::byte> output)
        -> libchiaking::Expected<void, platform::CryptoError> override {
      return full_.aes128_encrypt_block(key, block, output);
    }

    [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                   std::span<const std::byte> iv,
                                   std::span<const std::byte> aad,
                                   std::span<std::byte> tag)
        -> libchiaking::Expected<void, platform::CryptoError> override {
      return full_.aes128_gmac(key, iv, aad, tag);
    }

   private:
    test_support::SoftwareCrypto full_;
  };

  test_support::SoftwareCrypto crypto;
  MemoryStore store;
};

class KakusanSupportedPlatform : public KakusanPlatform {
 protected:
  FakeSecureDatagramProvider secure_datagram;
  test_support::SeededRandom random{5};
  platform::DefaultPlatformFactory factory{
      platform::PlatformProviders{.random = &random,
                                  .crypto = &crypto,
                                  .secure_datagram = &secure_datagram}};
};

TEST_F(KakusanPlatform, IsUnavailableWithoutSecureDatagrams) {
  platform::DefaultPlatformFactory bare{
      platform::PlatformProviders{.crypto = &crypto}};
  EXPECT_FALSE(bare.supplies(platform::PlatformCapability::secure_datagram));
  EXPECT_EQ(
      bare.info().capabilities & platform::PlatformCapability::secure_datagram,
      platform::PlatformCapability::none);
  const auto missing = ServiceBuilder::create<Kakusan>(bare, store);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), Unavailable::no_secure_transport);
  platform::DefaultPlatformFactory nothing;
  const auto none = ServiceBuilder::create<Kakusan>(nothing, store);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error(), Unavailable::no_secure_transport);
  EXPECT_EQ(Kakusan::name(Unavailable::no_secure_transport),
            "NO_SECURE_TRANSPORT");
  EXPECT_EQ(store.identity_saves, 0U);
}

TEST_F(KakusanPlatform, IsUnavailableWithoutIdentityCrypto) {
  FakeSecureDatagramProvider secure_datagram;
  platform::DefaultPlatformFactory absent{
      platform::PlatformProviders{.secure_datagram = &secure_datagram}};
  const auto without = ServiceBuilder::create<Kakusan>(absent, store);
  ASSERT_FALSE(without);
  EXPECT_EQ(without.error(), Unavailable::no_identity_crypto);
  PrimitivesOnly primitives;
  platform::DefaultPlatformFactory lacking{platform::PlatformProviders{
      .crypto = &primitives, .secure_datagram = &secure_datagram}};
  const auto probed = ServiceBuilder::create<Kakusan>(lacking, store);
  ASSERT_FALSE(probed);
  EXPECT_EQ(probed.error(), Unavailable::no_identity_crypto);
  EXPECT_EQ(Kakusan::name(Unavailable::no_identity_crypto),
            "NO_IDENTITY_CRYPTO");
  EXPECT_FALSE(Surroundings::has_identity_crypto(primitives));
  EXPECT_EQ(store.identity_saves, 0U);
  EXPECT_EQ(secure_datagram.opened(), 0U);
}

TEST_F(KakusanSupportedPlatform, MakesItsIdentityOnce) {
  EXPECT_EQ(factory.info().capabilities &
                platform::PlatformCapability::secure_datagram,
            platform::PlatformCapability::secure_datagram);
  const auto first = ServiceBuilder::create<Kakusan>(factory, store);
  ASSERT_TRUE(first);
  EXPECT_EQ(store.identity_saves, 1U);
  const auto key = first.value().this_device().key();
  EXPECT_EQ(first.value().this_device().fingerprint(),
            DeviceIdentity::fingerprint_of(crypto, key).value());
  const auto again = ServiceBuilder::create<Kakusan>(factory, store);
  ASSERT_TRUE(again);
  EXPECT_EQ(store.identity_saves, 1U);
  EXPECT_EQ(again.value().this_device().key(), key);
  EXPECT_EQ(secure_datagram.opened(), 0U);

  MemoryStore other;
  const auto stranger = ServiceBuilder::create<Kakusan>(factory, other);
  ASSERT_TRUE(stranger);
  EXPECT_NE(stranger.value().this_device().key(), key);
}

TEST_F(KakusanSupportedPlatform, RefusesTextThatIsNotAShortBlock) {
  const auto root = ServiceBuilder::create<Kakusan>(factory, store);
  ASSERT_TRUE(root);
  const auto unread = root.value().read("RCP1:x");
  ASSERT_FALSE(unread);
  EXPECT_EQ(unread.error(), Refusal::Kind::invalid_text);
  const auto empty = root.value().read({});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), Refusal::Kind::invalid_argument);
}

TEST_F(KakusanSupportedPlatform, IsUnavailableWithoutItsDeviceStore) {
  MemoryStore broken;
  broken.unavailable = true;
  const auto unsaved = ServiceBuilder::create<Kakusan>(factory, broken);
  ASSERT_FALSE(unsaved);
  EXPECT_EQ(unsaved.error(), Unavailable::device_store_unavailable);
  EXPECT_EQ(Kakusan::name(Unavailable::device_store_unavailable),
            "DEVICE_STORE_UNAVAILABLE");
  EXPECT_EQ(Kakusan::name(Unavailable::invalid_stun_servers),
            "INVALID_STUN_SERVERS");
}

class InjectedSurroundings : public KakusanPlatform {
 protected:
  test_support::SeededRandom random{1};
  LoopbackFactory factory{std::make_shared<LoopbackExchange>(crypto)};
  VirtualClock clock;
  MemoryStore given{TestPlatform::identity(0x22)};
};

TEST_F(InjectedSurroundings, NeedAClock) {
  EXPECT_FALSE(Surroundings::of(
      Surroundings::Injected{crypto, random, Surroundings::ClockSource{}, given,
                             TestPlatform::threading(), nullptr, &factory}));
}

TEST_F(InjectedSurroundings, NeedAWayOut) {
  const auto stranded = Surroundings::of(Surroundings::Injected{
      crypto, random, clock.source(), given, TestPlatform::threading()});
  ASSERT_FALSE(stranded);
  EXPECT_EQ(stranded.error(), Unavailable::no_secure_transport);
}

TEST_F(InjectedSurroundings, UseTheGivenIdentityClockAndFactory) {
  auto made = Surroundings::of(
      Surroundings::Injected{crypto, random, clock.source(), given,
                             TestPlatform::threading(), nullptr, &factory});
  ASSERT_TRUE(made);
  EXPECT_EQ(made.value()->now().now_us, 2'000'000U);
  EXPECT_EQ(made.value()->identity().key(), TestPlatform::identity(0x22).key());
  EXPECT_EQ(given.identity_saves, 0U);
  EXPECT_EQ(&made.value()->connections()->factory(), &factory);
  auto root = ServiceBuilder::on<Kakusan>(std::move(made).value());
  EXPECT_EQ(root.this_device().key(), TestPlatform::identity(0x22).key());
  EXPECT_TRUE(root.remembered().all().empty());
}

}  // namespace
}  // namespace kakusan_test
