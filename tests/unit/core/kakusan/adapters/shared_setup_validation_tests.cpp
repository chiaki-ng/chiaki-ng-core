#include "src/core/net/kakusan/control_message.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace kakusan_test {
namespace {

using libchiaking::net::kakusan::Error;

class SourceSetupValidation : public SharedSetupTest {
 protected:
  static auto refused(const SourceSetup& setup) -> ::testing::AssertionResult {
    return fails_with(setup.validate(), Error::codec);
  }

  const SourceSetup base_{fixture_setup()};
};

TEST_F(SourceSetupValidation, AcceptsTheFixture) {
  EXPECT_TRUE(base_.validate());
}

TEST_F(SourceSetupValidation, AcceptsProtocolVersionsNineToTwenty) {
  for (const unsigned version : {0U, 7U, 8U, 21U, 22U, 23U, 24U, 25U, 255U}) {
    SCOPED_TRACE(version);
    auto setup = base_.clone();
    setup.source_protocol = static_cast<std::uint8_t>(version);
    EXPECT_TRUE(refused(setup));
  }
  for (const unsigned version :
       {9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U}) {
    SCOPED_TRACE(version);
    auto setup = base_.clone();
    setup.source_protocol = static_cast<std::uint8_t>(version);
    EXPECT_TRUE(setup.validate());
  }
  auto setup = base_.clone();
  setup.source_protocol = 9;
  EXPECT_TRUE(setup.validate());
}

TEST_F(SourceSetupValidation, AcceptsFrameRatesUpToSixty) {
  for (const unsigned fps : {0U, 61U, 255U}) {
    SCOPED_TRACE(fps);
    auto setup = base_.clone();
    setup.video_fps = static_cast<std::uint8_t>(fps);
    EXPECT_TRUE(refused(setup));
  }
  for (const unsigned fps : {1U, 30U, 60U}) {
    SCOPED_TRACE(fps);
    auto setup = base_.clone();
    setup.video_fps = static_cast<std::uint8_t>(fps);
    EXPECT_TRUE(setup.validate());
  }
}

TEST_F(SourceSetupValidation, HoldsOneToEightProfiles) {
  auto setup = base_.clone();
  setup.video_profiles.clear();
  EXPECT_TRUE(refused(setup));
  for (std::size_t count = 1; count <= 8; ++count) {
    SCOPED_TRACE(count);
    setup = base_.clone();
    setup.video_profiles.assign(count, base_.video_profiles[0]);
    EXPECT_TRUE(setup.validate());
  }
  setup.video_profiles.assign(9, base_.video_profiles[0]);
  EXPECT_TRUE(refused(setup));
}

TEST_F(SourceSetupValidation, LimitsThePictureTo1080p) {
  for (const std::uint16_t width :
       {std::uint16_t{0}, std::uint16_t{1921}, std::uint16_t{65535}}) {
    SCOPED_TRACE(width);
    auto setup = base_.clone();
    setup.video_profiles[0].width = width;
    EXPECT_TRUE(refused(setup));
  }
  for (const std::uint16_t width : {std::uint16_t{1}, std::uint16_t{1920}}) {
    SCOPED_TRACE(width);
    auto setup = base_.clone();
    setup.video_profiles[0].width = width;
    EXPECT_TRUE(setup.validate());
  }
  for (const std::uint16_t height :
       {std::uint16_t{0}, std::uint16_t{1081}, std::uint16_t{65535}}) {
    SCOPED_TRACE(height);
    auto setup = base_.clone();
    setup.video_profiles[0].height = height;
    EXPECT_TRUE(refused(setup));
  }
  for (const std::uint16_t height : {std::uint16_t{1}, std::uint16_t{1080}}) {
    SCOPED_TRACE(height);
    auto setup = base_.clone();
    setup.video_profiles[0].height = height;
    EXPECT_TRUE(setup.validate());
  }
}

TEST_F(SourceSetupValidation, LimitsAProfileHeaderToOneTo512Bytes) {
  auto setup = base_.clone();
  setup.video_profiles[0].header.clear();
  EXPECT_TRUE(refused(setup));
  setup.video_profiles[0].header.assign(1, std::byte{1});
  EXPECT_TRUE(setup.validate());
  setup.video_profiles[0].header.assign(512, std::byte{1});
  EXPECT_TRUE(setup.validate());
  setup.video_profiles[0].header.assign(513, std::byte{1});
  EXPECT_TRUE(refused(setup));
}

TEST_F(SourceSetupValidation, ChecksTheAudioHeader) {
  auto setup = base_.clone();
  setup.audio_header = audio_header_with(3, 16, 48000, 960);
  EXPECT_TRUE(refused(setup));
}

}  // namespace
}  // namespace kakusan_test
