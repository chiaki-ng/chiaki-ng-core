#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace play_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
using Rig = LinkDriverRig;
using CloudService = Rig::CloudService;
using EcdhCurve = Rig::EcdhCurve;
using RegistrationTarget = Rig::RegistrationTarget;
using StreamState = Rig::StreamState;

class LinkDriverVersions : public ::testing::Test {
 protected:
  template <class Configuration>
  static void expect_version(Configuration configuration, std::uint32_t version,
                             EcdhCurve curve) {
    Rig::RealStream opened{std::move(configuration)};
    EXPECT_EQ(opened->takion_version(), version);
    EXPECT_EQ(opened->ecdh_curve(), curve);
    EXPECT_EQ(opened->state(), StreamState::idle);
  }

  template <class Configuration>
  static void expect_default(Configuration configuration, std::uint32_t base,
                             bool cloud_stream = false) {
    const auto version = Rig::pinned_version(base, cloud_stream);
    expect_version(std::move(configuration), version, Rig::curve_for(version));
  }
};

TEST_F(LinkDriverVersions, FollowTheHomeTargetAndItsOverride) {
  {
    SCOPED_TRACE("ps5");
    expect_default(Rig::local_for(RegistrationTarget::ps5_1),
                   takion::takion_version_ps5);
  }
  {
    SCOPED_TRACE("ps5 overridden to 12");
    expect_version(Rig::local_for(RegistrationTarget::ps5_1, 12), 12,
                   EcdhCurve::secp256k1);
  }
  {
    SCOPED_TRACE("ps5 overridden to 8");
    expect_default(Rig::local_for(RegistrationTarget::ps5_1, 8),
                   takion::takion_version_ps5);
  }
  {
    SCOPED_TRACE("ps5 overridden to 23");
    expect_default(Rig::local_for(RegistrationTarget::ps5_1, 23),
                   takion::takion_version_ps5);
  }
  {
    SCOPED_TRACE("ps4");
    expect_default(Rig::local_for(RegistrationTarget::ps4_10),
                   takion::takion_version_ps4);
  }
  {
    SCOPED_TRACE("ps4 overridden to 10");
    expect_version(Rig::local_for(RegistrationTarget::ps4_10, 10), 10,
                   EcdhCurve::secp521r1);
  }
}

TEST_F(LinkDriverVersions, FollowTheInternetTargetAndItsOverride) {
  {
    SCOPED_TRACE("ps5");
    expect_default(Rig::remote_for(RegistrationTarget::ps5_1),
                   takion::takion_version_ps5);
  }
  {
    SCOPED_TRACE("ps5 overridden to 20");
    expect_version(Rig::remote_for(RegistrationTarget::ps5_1, 20), 20,
                   EcdhCurve::secp521r1);
  }
  {
    SCOPED_TRACE("ps5 overridden to 25");
    expect_default(Rig::remote_for(RegistrationTarget::ps5_1, 25),
                   takion::takion_version_ps5);
  }
  {
    SCOPED_TRACE("ps4");
    expect_default(Rig::remote_for(RegistrationTarget::ps4_10),
                   takion::takion_version_ps4);
  }
}

TEST_F(LinkDriverVersions, FollowTheCloudServiceAndItsOverride) {
  {
    SCOPED_TRACE("cloud stream");
    expect_default(Rig::cloud_for(CloudService::cloud_stream),
                   takion::takion_version_cloud_stream, true);
  }
  {
    SCOPED_TRACE("cloud stream overridden to 13");
    expect_version(Rig::cloud_for(CloudService::cloud_stream, 13), 13,
                   EcdhCurve::secp521r1);
  }
  {
    SCOPED_TRACE("cloud stream overridden to 25");
    expect_version(Rig::cloud_for(CloudService::cloud_stream, 25), 25,
                   EcdhCurve::secp521r1);
  }
  {
    SCOPED_TRACE("legacy cloud stream");
    expect_default(Rig::cloud_for(CloudService::legacy_cloud_stream),
                   takion::takion_version_legacy_cloud_stream);
  }
  {
    SCOPED_TRACE("legacy cloud stream overridden past every version");
    expect_default(
        Rig::cloud_for(CloudService::legacy_cloud_stream, 0xffffffffU),
        takion::takion_version_legacy_cloud_stream);
  }
  {
    SCOPED_TRACE("legacy cloud stream overridden to 21");
    expect_default(Rig::cloud_for(CloudService::legacy_cloud_stream, 21),
                   takion::takion_version_legacy_cloud_stream);
  }
}

TEST_F(LinkDriverVersions, MatchTheBuildsVersionForEachTarget) {
  using takion::BuildTarget;
  const auto ps4 = takion::takion_build_version(BuildTarget::ps4);
  const auto ps5 = takion::takion_build_version(BuildTarget::ps5);
  const auto legacy_cloud_stream =
      takion::takion_build_version(BuildTarget::legacy_cloud_stream);
  const auto cloud_stream =
      takion::takion_build_version(BuildTarget::cloud_stream);
  EXPECT_EQ(ps4, Rig::pinned_version(takion::takion_version_ps4, false));
  EXPECT_EQ(ps5, Rig::pinned_version(takion::takion_version_ps5, false));
  EXPECT_EQ(
      legacy_cloud_stream,
      Rig::pinned_version(takion::takion_version_legacy_cloud_stream, false));
  EXPECT_EQ(cloud_stream,
            Rig::pinned_version(takion::takion_version_cloud_stream, true));
  if (takion::takion_build_version_override == 0) {
    EXPECT_EQ(ps4, takion::takion_version_ps4);
    EXPECT_EQ(ps5, takion::takion_version_ps5);
    EXPECT_EQ(legacy_cloud_stream, takion::takion_version_legacy_cloud_stream);
    EXPECT_EQ(cloud_stream, takion::takion_version_cloud_stream);
  }
  expect_version(Rig::local_for(RegistrationTarget::ps4_10), ps4,
                 Rig::curve_for(ps4));
  expect_version(Rig::local_for(RegistrationTarget::ps5_1), ps5,
                 Rig::curve_for(ps5));
  expect_version(Rig::remote_for(RegistrationTarget::ps5_1), ps5,
                 Rig::curve_for(ps5));
  expect_version(Rig::cloud_for(CloudService::legacy_cloud_stream),
                 legacy_cloud_stream, Rig::curve_for(legacy_cloud_stream));
  expect_version(Rig::cloud_for(CloudService::cloud_stream), cloud_stream,
                 Rig::curve_for(cloud_stream));
}

TEST_F(LinkDriverVersions, KeepTheReportedVersionWhenNegotiating) {
  auto local = Rig::local_for(RegistrationTarget::ps5_1);
  local.tuning.negotiate_takion_version = true;
  expect_default(std::move(local), takion::takion_version_ps5);
  auto remote = Rig::remote_for(RegistrationTarget::ps4_10);
  remote.tuning.negotiate_takion_version = true;
  expect_default(std::move(remote), takion::takion_version_ps4);
  auto cloud_stream = Rig::cloud_for(CloudService::cloud_stream);
  cloud_stream.tuning.negotiate_takion_version = true;
  expect_default(std::move(cloud_stream), takion::takion_version_cloud_stream,
                 true);
  auto legacy_cloud_stream = Rig::cloud_for(CloudService::legacy_cloud_stream);
  legacy_cloud_stream.tuning.negotiate_takion_version = true;
  expect_default(std::move(legacy_cloud_stream),
                 takion::takion_version_legacy_cloud_stream);
}

}  // namespace
}  // namespace play_test
