#include "libchiaking/platform/default_factory.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/cloud/cloud_link_rig.hpp"
#include "tests/unit/core/play/cloud_provision.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using cloud_links::AudioHttp;
using cloud_links::RejectCrypto;
using cloud_links::StateSink;
using cloud_links::UnauthorizedHttp;
using libchiaking::catalog::ServiceGeneration;
using libchiaking::cloud::CloudService;

class CloudAudioRequest : public cloud_links::CloudLinkTest {};

TEST_F(CloudAudioRequest, CarriesTheSelectedLayoutIntoTheStartRequest) {
  constexpr std::array<std::pair<std::uint64_t, std::string_view>, 5> choices{
      {{0, "7.1.4"}, {2, "2"}, {6, "5.1"}, {8, "7.1"}, {12, "7.1.4"}}};
  for (const auto& [channels, layout] : choices) {
    SCOPED_TRACE(channels);
    RejectCrypto crypto;
    AudioHttp http;
    platform::DefaultPlatformFactory providers{
        platform::PlatformProviders{.crypto = &crypto, .http = &http}};
    auto config = make_config();
    config.datacenter = play::DatacenterPreference::named(
        play::DatacenterName::parse("dc1").value());
    config.tuning.requested_audio_channels = channels;
    const auto provisioned =
        CloudProvision::run(std::move(config), providers, cleared().seams());
    EXPECT_TRUE(provisioned);
    EXPECT_EQ(http.ps.starts, 1U);
    EXPECT_EQ(http.ps.unscripted, 0U);
    EXPECT_EQ(http.selected, layout);
    EXPECT_EQ(http.preferred, layout);
    EXPECT_EQ(http.maximum, "7.1.4");
  }
}

TEST_F(CloudAudioRequest, RefusesSurroundOnTheLegacyServiceBeforeAnyRequest) {
  RejectCrypto crypto;
  UnauthorizedHttp http;
  platform::DefaultPlatformFactory providers{
      platform::PlatformProviders{.crypto = &crypto, .http = &http}};
  for (const auto service :
       {CloudService::cloud_stream, CloudService::legacy_cloud_stream}) {
    for (const auto channels :
         std::initializer_list<std::uint8_t>{0, 2, 6, 8, 12}) {
      for (const bool allocated : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "service " << static_cast<int>(service) << " channels "
                     << static_cast<int>(channels) << " allocated "
                     << allocated);
        auto gated_config = make_config(service);
        if (allocated) {
          gated_config.allocation = play::CloudAllocation{};
          gated_config.allocation->service = service;
        }
        gated_config.tuning.requested_audio_channels = channels;
        auto stop = std::make_shared<test_support::AtomicStop>();
        stop->stopped = true;
        gated_config.stop = platform::StopToken{stop};
        auto gated = play::CloudLinkConfiguration::link(
            std::move(gated_config), providers, cleared(service).seams());
        ASSERT_TRUE(gated);
        StateSink gated_sink;
        gated.value()->set_event_sink(gated_sink);
        const auto gated_start = gated.value()->start();
        EXPECT_FALSE(gated_start);
        const auto& failure = gated.value()->failure();
        ASSERT_TRUE(failure);
        if (channels <= 2 || service == CloudService::cloud_stream) {
          EXPECT_FALSE(failure->as<play::LinkFailure::AudioRequestRefused>());
          continue;
        }
        ASSERT_FALSE(gated_start);
        EXPECT_EQ(gated_start.error(),
                  remote_play::StreamError::invalid_configuration);
        EXPECT_EQ(gated.value()->state(), remote_play::StreamState::idle);
        EXPECT_EQ(gated_sink.states, 0U);
        EXPECT_EQ(gated_sink.failures, 0U);
        EXPECT_EQ(http.calls(), 0U);
        EXPECT_EQ(failure->route(),
                  play::Venue::cloud(ServiceGeneration::old_service));
        EXPECT_TRUE(failure->as<play::LinkFailure::AudioRequestRefused>());
        EXPECT_EQ(failure->describe(),
                  "surround audio is not supported for this target");
      }
    }
  }
}

}  // namespace
}  // namespace play_test
