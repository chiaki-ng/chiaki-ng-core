#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using Rig = LinkDriverRig;
using CloudService = Rig::CloudService;
using RegistrationTarget = Rig::RegistrationTarget;
using StreamError = Rig::StreamError;
using StreamState = Rig::StreamState;
using remote_play::AudioRequestTarget;

class LinkDriverSurround : public ::testing::Test {
 protected:
  static constexpr std::array<std::uint32_t, 5> channel_counts{0, 2, 6, 8, 12};

  template <class Configuration>
  static void expect_start_rejects_surround(
      Configuration configuration,
      std::optional<libchiaking::play::Venue> route) {
    Rig::RealStream opened{std::move(configuration)};
    EXPECT_TRUE(
        Rig::refused(opened->start(), StreamError::invalid_configuration));
    EXPECT_EQ(opened->state(), StreamState::idle);
    const auto& failure = opened->failure();
    ASSERT_TRUE(failure);
    EXPECT_FALSE(failure->cancelled());
    EXPECT_EQ(failure->route(), route);
    EXPECT_EQ(failure->result(), StreamError::invalid_configuration);
    EXPECT_TRUE(
        failure->as<libchiaking::play::LinkFailure::AudioRequestRefused>());
    EXPECT_NE(failure->sentence().find(
                  "surround audio is not supported for this target"),
              std::string::npos);
    EXPECT_EQ(opened.events.state_events, 0U);
  }

  static auto audio_for(RegistrationTarget target) -> AudioRequestTarget {
    return target == RegistrationTarget::ps5_1 ? AudioRequestTarget::ps5
                                               : AudioRequestTarget::ps4;
  }
};

TEST_F(LinkDriverSurround, FailsAHomeStartTheTargetCannotPlay) {
  for (const auto channels : channel_counts) {
    for (const auto target :
         {RegistrationTarget::ps4_8, RegistrationTarget::ps4_9,
          RegistrationTarget::ps4_10, RegistrationTarget::ps5_1}) {
      SCOPED_TRACE(testing::Message() << channels << " channels to "
                                      << testing::PrintToString(target));
      auto configuration = Rig::local_for(target);
      configuration.tuning.requested_audio_channels = channels;
      if (!remote_play::requested_audio_channels_supported(audio_for(target),
                                                           channels))
        expect_start_rejects_surround(std::move(configuration),
                                      libchiaking::play::Venue::home());
      else
        EXPECT_TRUE(std::move(configuration).validated());
    }
  }
}

TEST_F(LinkDriverSurround, FailsAnInternetStartTheTargetCannotPlay) {
  for (const auto channels : channel_counts) {
    for (const auto target :
         {RegistrationTarget::ps4_10, RegistrationTarget::ps5_1}) {
      SCOPED_TRACE(testing::Message() << channels << " channels to "
                                      << testing::PrintToString(target));
      auto configuration = Rig::remote_for(target);
      configuration.tuning.requested_audio_channels = channels;
      if (!remote_play::requested_audio_channels_supported(audio_for(target),
                                                           channels))
        expect_start_rejects_surround(std::move(configuration),
                                      libchiaking::play::Venue::internet());
      else
        EXPECT_TRUE(std::move(configuration).validated());
    }
  }
}

TEST_F(LinkDriverSurround, FailsOnlyALegacyCloudStartAskingForSurround) {
  for (const auto channels : channel_counts) {
    for (const auto service :
         {CloudService::cloud_stream, CloudService::legacy_cloud_stream}) {
      SCOPED_TRACE(testing::Message() << channels << " channels to "
                                      << testing::PrintToString(service));
      const bool legacy_cloud_stream =
          service == CloudService::legacy_cloud_stream;
      auto configuration = Rig::cloud_for(service);
      configuration.tuning.requested_audio_channels = channels;
      const bool surround = channels > 2;
      EXPECT_NE(
          remote_play::requested_audio_channels_supported(
              legacy_cloud_stream ? AudioRequestTarget::legacy_cloud_stream
                                  : AudioRequestTarget::cloud_stream,
              channels),
          surround && legacy_cloud_stream);
      if (surround && legacy_cloud_stream)
        expect_start_rejects_surround(std::move(configuration),
                                      Rig::cloud_venue(service));
      else
        EXPECT_TRUE(std::move(configuration).validated());
    }
  }
}

}  // namespace
}  // namespace play_test
