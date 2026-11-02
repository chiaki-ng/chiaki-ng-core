#include "libchiaking/detail/kakusan/device_identity.hpp"

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/identities.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::DeviceIdentity;
using libchiaking::kakusan::SavedIdentity;
using test_support::Bytes;
using test_support::kakusan::Fixtures;

static_assert(!std::is_copy_constructible_v<DeviceIdentity>);
static_assert(!std::is_copy_assignable_v<DeviceIdentity>);
static_assert(std::is_nothrow_move_constructible_v<DeviceIdentity>);
static_assert(std::is_nothrow_move_assignable_v<DeviceIdentity>);
static_assert(!std::is_copy_constructible_v<SavedIdentity>);

class DeviceIdentityTest : public ::testing::Test {
 protected:
  static auto is_zeroed(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(
        bytes, [](std::byte value) { return value == std::byte{}; });
  }

  static auto seed_of(const DeviceIdentity& identity) -> std::string {
    const auto saved = identity.saved();
    return Bytes::hex(
        std::span<const std::byte>{saved.seed.data(), saved.seed.size()});
  }

  static auto zeroed_seed(const DeviceIdentity& identity) -> bool {
    const auto saved = identity.saved();
    return is_zeroed(
        std::span<const std::byte>{saved.seed.data(), saved.seed.size()});
  }

  auto from_vector(std::string_view name) -> DeviceIdentity {
    return Identities::from_seed(crypto,
                                 Bytes::unhex(Fixtures::string(vectors, name)));
  }

  test_support::SoftwareCrypto crypto;
  std::string vectors{Fixtures::text("vectors.json")};
};

class DeviceIdentitySeed : public DeviceIdentityTest {
 protected:
  std::string seed{Fixtures::string(vectors, "host_seed")};
  std::string expected{Fixtures::string(vectors, "A").substr(22)};
};

TEST_F(DeviceIdentitySeed, DerivesTheFixtureKey) {
  const auto identity = from_vector("host_seed");
  EXPECT_EQ(Bytes::hex(identity.key().bytes()), expected);
  EXPECT_EQ(seed_of(identity), seed);
}

TEST_F(DeviceIdentitySeed, GivesTheHintTheInvitationCarries) {
  const auto identity = from_vector("host_seed");
  const auto invitation =
      libchiaking::net::kakusan::ShortBlockCodec::parse_invitation(
          crypto, Fixtures::text("invitation.txt"));
  ASSERT_TRUE(invitation);
  EXPECT_EQ(invitation.value().body.identity_hint,
            libchiaking::net::kakusan::ShortBlockCodec::hint_of(
                identity.key().bytes()));
}

TEST_F(DeviceIdentitySeed, MovesOutAndLeavesTheSourceWiped) {
  auto identity = from_vector("host_seed");
  auto moved = std::move(identity);
  EXPECT_EQ(seed_of(moved), seed);
  EXPECT_EQ(Bytes::hex(moved.key().bytes()), expected);
  EXPECT_TRUE(zeroed_seed(identity));

  auto assigned = from_vector("host_seed");
  assigned = std::move(moved);
  EXPECT_EQ(seed_of(assigned), seed);
  EXPECT_EQ(Bytes::hex(assigned.key().bytes()), expected);
  EXPECT_TRUE(zeroed_seed(moved));
}

TEST_F(DeviceIdentitySeed, ClonesTheKeyFingerprintAndSeed) {
  const auto identity = from_vector("host_seed");
  const auto copy = identity.clone();
  EXPECT_EQ(copy.key(), identity.key());
  EXPECT_EQ(copy.fingerprint(), identity.fingerprint());
  EXPECT_EQ(seed_of(copy), seed_of(identity));
}

TEST_F(DeviceIdentityTest, FingerprintIsTheKeyDigest) {
  const auto identity = from_vector("guest_seed");
  const auto digest =
      libchiaking::net::kakusan::Hash::sha256(crypto, identity.key().bytes());
  const auto print = DeviceIdentity::fingerprint_of(crypto, identity.key());
  ASSERT_TRUE(print);
  ASSERT_TRUE(digest);
  EXPECT_EQ(print.value().bytes(), digest.value());
  EXPECT_EQ(identity.fingerprint(), print.value());
}

TEST_F(DeviceIdentityTest, NewIdentitiesAreDrawnFromTheRandomSource) {
  test_support::SeededRandom random{7};
  const auto first = DeviceIdentity::create(crypto, random);
  const auto second = DeviceIdentity::create(crypto, random);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first.value().key(), second.value().key());
  EXPECT_NE(first.value().fingerprint(), second.value().fingerprint());
  const auto restored = DeviceIdentity::restore(crypto, first.value().saved());
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored.value().key(), first.value().key());
  EXPECT_EQ(restored.value().fingerprint(), first.value().fingerprint());
}

}  // namespace
}  // namespace kakusan_test
