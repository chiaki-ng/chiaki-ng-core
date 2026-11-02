#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace play_test {
namespace {

using libchiaking::catalog::ServiceGeneration;
using libchiaking::consoles::Generation;
using libchiaking::play::DeveloperTuning;

class DeveloperTunings : public ::testing::Test {
 protected:
  static auto contains(const std::vector<std::uint32_t>& versions,
                       std::uint32_t version) -> bool {
    return std::ranges::find(versions, version) != versions.end();
  }
};

TEST_F(DeveloperTunings, ChangeNothingByDefault) {
  const DeveloperTuning standard;
  EXPECT_FALSE(standard.disables_audio());
  EXPECT_FALSE(standard.disables_video());
  EXPECT_FALSE(standard.protocol_version());
  EXPECT_FALSE(standard.negotiates_protocol_version());
  EXPECT_EQ(standard.packet_loss_max(), 0.05);
  EXPECT_FALSE(standard.keyframe_on_lost_frame());
  EXPECT_EQ(standard.nat_probe_sockets(), 250u);
  EXPECT_EQ(*DeveloperTuning::create({}), standard);
}

TEST_F(DeveloperTunings, KeepEveryValueGiven) {
  const auto tuned =
      DeveloperTuning::create({.disable_audio = true,
                               .disable_video = true,
                               .protocol_version = 12,
                               .negotiate_protocol_version = true,
                               .packet_loss_max = 1.0,
                               .keyframe_on_lost_frame = false,
                               .nat_probe_sockets = 4096});
  ASSERT_TRUE(tuned);
  EXPECT_TRUE(tuned->disables_audio());
  EXPECT_TRUE(tuned->disables_video());
  EXPECT_EQ(tuned->protocol_version(), 12U);
  EXPECT_TRUE(tuned->negotiates_protocol_version());
  EXPECT_EQ(tuned->packet_loss_max(), 1.0);
  EXPECT_EQ(tuned->keyframe_on_lost_frame(), false);
  EXPECT_EQ(tuned->nat_probe_sockets(), 4096u);
}

TEST_F(DeveloperTunings, RefuseValuesOutOfRange) {
  EXPECT_TRUE(DeveloperTuning::create({.packet_loss_max = 0.0}));
  EXPECT_FALSE(DeveloperTuning::create({.packet_loss_max = -0.01}));
  EXPECT_FALSE(DeveloperTuning::create({.packet_loss_max = 1.01}));
  EXPECT_FALSE(DeveloperTuning::create({.protocol_version = 0}));
  EXPECT_FALSE(DeveloperTuning::create({.nat_probe_sockets = 4097}));
}

TEST_F(DeveloperTunings, NameTheBuiltProtocolVersionOfEachTarget) {
  for (const auto generation : {Generation::ps4, Generation::ps5}) {
    SCOPED_TRACE(testing::PrintToString(generation));
    const auto version = DeveloperTuning::built_protocol_version(generation);
    EXPECT_NE(version, 0u);
    EXPECT_TRUE(DeveloperTuning::create({.protocol_version = version}));
  }
  for (const auto service :
       {ServiceGeneration::old_service, ServiceGeneration::ps5_cloud}) {
    SCOPED_TRACE(testing::PrintToString(service));
    const auto version = DeveloperTuning::built_protocol_version(service);
    EXPECT_NE(version, 0u);
    EXPECT_TRUE(DeveloperTuning::create({.protocol_version = version}));
  }
  const auto forced = DeveloperTuning::build_protocol_override();
  if (forced) {
    EXPECT_NE(*forced, 0u);
  }
}

TEST_F(DeveloperTunings, ListTheConsoleProtocolVersionsTheBuildImplements) {
  for (const auto generation : {Generation::ps4, Generation::ps5}) {
    SCOPED_TRACE(testing::PrintToString(generation));
    const auto versions =
        DeveloperTuning::implemented_protocol_versions(generation);
    EXPECT_TRUE(std::ranges::is_sorted(versions));
    EXPECT_TRUE(contains(versions,
                         DeveloperTuning::built_protocol_version(generation)));
    EXPECT_TRUE(contains(versions, 9));
    EXPECT_TRUE(contains(versions, 20));
    EXPECT_FALSE(contains(versions, 21));
    EXPECT_FALSE(contains(versions, 23));
    EXPECT_FALSE(contains(versions, 25));
  }
}

TEST_F(DeveloperTunings, ListTheCloudProtocolVersionsTheBuildImplements) {
  const auto legacy = DeveloperTuning::implemented_protocol_versions(
      ServiceGeneration::old_service);
  EXPECT_EQ(legacy,
            DeveloperTuning::implemented_protocol_versions(Generation::ps4));
  EXPECT_TRUE(contains(legacy, DeveloperTuning::built_protocol_version(
                                   ServiceGeneration::old_service)));
  const auto ps5_cloud = DeveloperTuning::implemented_protocol_versions(
      ServiceGeneration::ps5_cloud);
  EXPECT_TRUE(std::ranges::is_sorted(ps5_cloud));
  EXPECT_TRUE(contains(ps5_cloud, 21));
  EXPECT_TRUE(contains(ps5_cloud, 23));
  EXPECT_TRUE(contains(ps5_cloud, 25));
  EXPECT_FALSE(contains(ps5_cloud, 22));
  EXPECT_TRUE(contains(ps5_cloud, DeveloperTuning::built_protocol_version(
                                      ServiceGeneration::ps5_cloud)));
  for (const auto version : ps5_cloud) {
    SCOPED_TRACE(version);
    EXPECT_TRUE(DeveloperTuning::create({.protocol_version = version}));
  }
}

}  // namespace
}  // namespace play_test
