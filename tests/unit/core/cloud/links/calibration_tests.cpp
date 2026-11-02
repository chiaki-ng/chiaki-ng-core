#include "tests/unit/core/cloud/links/connect_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace cloud_test {
namespace {

using Reason = ConnectRig::Reason;
using Stage = ConnectRig::Stage;

class CloudCalibration : public PinnedTakionVersions {
 protected:
  void SetUp() override {
    PinnedTakionVersions::SetUp();
    row.measured = true;
    row.rtt_ms = 37;
    row.mtu_in = 1454;
    row.mtu_out = 1254;
  }

  auto legacy(std::uint32_t version = 0) const {
    return play::Calibration::from_ping(row, CloudService::legacy_cloud_stream,
                                        version);
  }

  auto cloud(std::uint32_t version = 0) const {
    return play::Calibration::from_ping(row, CloudService::cloud_stream,
                                        version);
  }

  static auto takes_version(
      const libchiaking::Expected<play::Calibration, play::ConnectError>&
          calibration,
      std::uint32_t version) -> testing::AssertionResult {
    if (!calibration)
      return testing::AssertionFailure() << "the calibration failed";
    if (calibration.value().measurement().takion_version != version)
      return testing::AssertionFailure()
             << "took version "
             << calibration.value().measurement().takion_version;
    return testing::AssertionSuccess();
  }

  libchiaking::psn::gk::Ping row;
};

TEST_F(CloudCalibration, MeasuresTheLegacyServiceAtItsVersion) {
  const auto result = legacy();
  EXPECT_TRUE(
      takes_version(result, takion::takion_version_legacy_cloud_stream));
  ASSERT_TRUE(result);
  const auto& measurement = result.value().measurement();
  EXPECT_EQ(measurement.wire_profile.version(),
            takion::takion_version_legacy_cloud_stream);
  EXPECT_EQ(measurement.round_trip_time.adjusted_mean,
            std::chrono::microseconds{17'000});
  EXPECT_EQ(measurement.inbound_mtu, 1454);
  EXPECT_EQ(measurement.outbound_mtu, 1254);
}

TEST_F(CloudCalibration, MeasuresCloudStreamAtItsVersion) {
  const auto cloud_stream = cloud();
  EXPECT_TRUE(takes_version(cloud_stream, takion::takion_version_cloud_stream));
  ASSERT_TRUE(cloud_stream);
  const auto& profile = cloud_stream.value().measurement().wire_profile;
  EXPECT_EQ(profile.version(), takion::takion_version_cloud_stream);
  EXPECT_TRUE(takion::takion_version_cloud_stream != 23 ||
              (profile.ecdh_curve() == platform::EcdhCurve::secp521r1 &&
               profile.av_layout() == takion::AvLayout::versions_22_to_23));
}

TEST_F(CloudCalibration, AssumesTheDefaultRoundTripWithoutOne) {
  row.rtt_ms = 0;
  const auto result = legacy();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().measurement().round_trip_time.adjusted_mean,
            std::chrono::microseconds{25'000});
}

TEST_F(CloudCalibration, RefusesAnUnmeasuredRow) {
  row.measured = false;
  EXPECT_TRUE(
      ConnectRig::fails_with(legacy(), Stage::route, Reason::not_measured));
}

TEST_F(CloudCalibration, ReplacesImpossibleMeasurements) {
  row.rtt_ms = -1;
  row.mtu_in = 0;
  row.mtu_out = -5;
  const auto result = legacy();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().measurement().round_trip_time.adjusted_mean,
            std::chrono::microseconds{25'000});
  EXPECT_EQ(result.value().measurement().inbound_mtu, 1454);
  EXPECT_EQ(result.value().measurement().outbound_mtu, 1254);
}

TEST_F(CloudCalibration, KeepsAMeasuredInboundMtu) {
  row.rtt_ms = 17;
  row.mtu_in = 1400;
  row.mtu_out = 0;
  const auto result = legacy();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().measurement().inbound_mtu, 1400);
  EXPECT_EQ(result.value().measurement().outbound_mtu, 1254);
}

TEST_F(CloudCalibration, TakesAnOverriddenCloudVersionWithItsProfile) {
  const auto overridden = cloud(13);
  EXPECT_TRUE(takes_version(overridden, 13));
  ASSERT_TRUE(overridden);
  EXPECT_EQ(overridden.value().measurement().wire_profile.version(), 13U);
  EXPECT_EQ(overridden.value().measurement().wire_profile.ecdh_curve(),
            platform::EcdhCurve::secp521r1);
}

TEST_F(CloudCalibration, KeepsTheLegacyVersionForAnUnknownOverride) {
  EXPECT_TRUE(
      takes_version(legacy(8), takion::takion_version_legacy_cloud_stream));
}

TEST_F(CloudCalibration, TakesTwentyForTheLegacyService) {
  EXPECT_TRUE(takes_version(legacy(20), 20));
}

TEST_F(CloudCalibration, KeepsCloudOnlyVersionsOffTheLegacyService) {
  for (const std::uint32_t version : {21U, 23U, 25U}) {
    SCOPED_TRACE(version);
    EXPECT_TRUE(takes_version(legacy(version),
                              takion::takion_version_legacy_cloud_stream));
    EXPECT_EQ(play::Calibration::takion_version(
                  CloudService::legacy_cloud_stream, version),
              takion::takion_version_legacy_cloud_stream);
  }
}

TEST_F(CloudCalibration, TakesCloudOnlyVersionsForCloudStream) {
  for (const std::uint32_t version : {21U, 23U, 25U}) {
    SCOPED_TRACE(version);
    const auto overridden = cloud(version);
    EXPECT_TRUE(takes_version(overridden, version));
    ASSERT_TRUE(overridden);
    EXPECT_EQ(overridden.value().measurement().wire_profile.version(), version);
    EXPECT_EQ(
        play::Calibration::takion_version(CloudService::cloud_stream, version),
        version);
  }
}

TEST_F(CloudCalibration, KeepsTheCloudVersionForAnUnimplementedOverride) {
  EXPECT_TRUE(takes_version(cloud(22), takion::takion_version_cloud_stream));
  EXPECT_TRUE(takes_version(cloud(24), takion::takion_version_cloud_stream));
}

TEST_F(CloudCalibration, AssumesDefaultsForAnUnmeasuredAllocation) {
  const auto unmeasured = play::Calibration::from_allocation(
      CloudService::cloud_stream, 0, 0, 1254, 40'000);
  ASSERT_TRUE(unmeasured);
  EXPECT_EQ(unmeasured.value().measurement().inbound_mtu, 1454);
  EXPECT_EQ(unmeasured.value().measurement().outbound_mtu, 1254);
  EXPECT_EQ(unmeasured.value().measurement().round_trip_time.adjusted_mean,
            std::chrono::microseconds{1000});
}

TEST_F(CloudCalibration, TakesTheRoundTripOfAMeasuredAllocation) {
  const auto measured = play::Calibration::from_allocation(
      CloudService::cloud_stream, 0, 1400, 1200, 40'000);
  ASSERT_TRUE(measured);
  EXPECT_EQ(measured.value().measurement().round_trip_time.adjusted_mean,
            std::chrono::microseconds{40'000});
  const auto no_rtt = play::Calibration::from_allocation(
      CloudService::cloud_stream, 0, 1400, 1200, 0);
  ASSERT_TRUE(no_rtt);
  EXPECT_EQ(no_rtt.value().measurement().round_trip_time.adjusted_mean,
            std::chrono::microseconds{1000});
}

}  // namespace
}  // namespace cloud_test
