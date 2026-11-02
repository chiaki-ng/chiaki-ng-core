#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

TEST(TakionVersionRegistry, ListsEveryImplementedVersionInOrder) {
  constexpr std::array<std::uint32_t, 16> known_versions{
      7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 25};
  std::size_t next_known_version = 0;
  for (const auto& registration : takion::takion_version_registry) {
    ASSERT_LT(next_known_version, known_versions.size());
    EXPECT_EQ(registration.version, known_versions[next_known_version]);
    EXPECT_TRUE(takion::is_implemented_takion_version(registration.version));
    EXPECT_TRUE(
        takion::resolve_takion_wire_profile(registration.version).has_value());
    ++next_known_version;
  }
  EXPECT_EQ(next_known_version, known_versions.size());
}

TEST(TakionVersionRegistry, ResolvesOnlyImplementedVersions) {
  for (std::uint32_t version = 0; version <= 64; ++version) {
    const bool known = version == 7 || (version >= 9 && version <= 21) ||
                       version == 23 || version == 25;
    EXPECT_EQ(takion::is_implemented_takion_version(version), known)
        << "version " << version;
    EXPECT_EQ(takion::resolve_takion_wire_profile(version).has_value(), known)
        << "version " << version;
  }
}

TEST(TakionVersionRegistry, RefusesTheGapsAndTheFarFuture) {
  for (const auto version :
       {std::uint32_t{22}, std::uint32_t{24}, std::uint32_t{26},
        std::uint32_t{31}, std::uint32_t{32}, std::uint32_t{64},
        std::uint32_t{0xffffffff}}) {
    EXPECT_FALSE(takion::is_implemented_takion_version(version))
        << "version " << version;
    EXPECT_FALSE(takion::resolve_takion_wire_profile(version).has_value())
        << "version " << version;
  }
}

TEST(TakionNegotiationOffer, ForCloudStreamReachesVersion25) {
  EXPECT_EQ(
      takion::takion_negotiation_offer(takion::SessionScope::cloud_stream),
      (std::vector<std::uint32_t>{9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                  23, 25}));
}

TEST(TakionNegotiationOffer, ForRemotePlayStopsAtVersion20) {
  const std::vector<std::uint32_t> remote_play{9,  10, 11, 13, 14, 15,
                                               16, 17, 18, 19, 20};
  EXPECT_EQ(takion::takion_negotiation_offer(
                takion::SessionScope::legacy_cloud_stream),
            remote_play);
  EXPECT_EQ(takion::takion_negotiation_offer(takion::SessionScope::console),
            remote_play);
}

}  // namespace
}  // namespace takion_test
