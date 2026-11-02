#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/cloud_provision.hpp"
#include "tests/unit/core/play/default_platform/adapters/links/link_driver_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using Rig = LinkDriverRig;
using RealStream = Rig::RealStream;
using CloudService = Rig::CloudService;
using StreamError = Rig::StreamError;
using StreamState = Rig::StreamState;
using remote_play::QuitReason;
using namespace std::chrono_literals;

class LinkDriverCancelledStart : public ::testing::Test {
 protected:
  template <class Configuration>
  static void fails_the_route_once(Configuration configuration,
                                   std::string_view prefix) {
    RealStream opened{std::move(configuration)};
    EXPECT_EQ(opened->state(), StreamState::idle);
    opened->cancel();
    const auto before = LiveProviders::steady_us();
    EXPECT_TRUE(opened->wait(5000ms));
    EXPECT_LT(Rig::since(before), 1000ms);
    EXPECT_TRUE(Rig::refused(opened->start(), StreamError::route_failed));
    EXPECT_EQ(opened.events.failed_events, 1U);
    ASSERT_FALSE(opened.events.failures.empty());
    EXPECT_EQ(opened.events.failures.back().reason,
              remote_play::StreamFailure::cancelled);
    EXPECT_EQ(opened.events.state_events, 0U);
    EXPECT_EQ(opened->state(), StreamState::failed);

    const auto& failure = opened->failure();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->result(), StreamError::route_failed);
    EXPECT_TRUE(failure->cancelled());
    EXPECT_EQ(failure->quit().reason, QuitReason::stopped);
    EXPECT_TRUE(failure->quit().text.empty());
    EXPECT_TRUE(failure->message().empty());
    const auto sentence = failure->sentence();
    EXPECT_NE(sentence.find(prefix), std::string::npos) << sentence;
    EXPECT_EQ(sentence.find("SYNTHETIC"), std::string::npos) << sentence;
    EXPECT_EQ(sentence.find(RealStream::entitlement), std::string::npos)
        << sentence;
    const auto kept = *failure;

    EXPECT_TRUE(opened->stop());
    const auto& after = opened->failure();
    ASSERT_TRUE(after);
    EXPECT_EQ(after->sentence(), sentence);
    EXPECT_EQ(after->route(), kept.route());
    EXPECT_EQ(after->stage(), kept.stage());
    EXPECT_TRUE(after->cancelled());
    EXPECT_EQ(after->quit().reason, kept.quit().reason);
    EXPECT_FALSE(opened->cleanup_failure());
    EXPECT_EQ(opened.events.state_events, 0U);
    EXPECT_EQ(opened.events.failed_events, 1U);
    EXPECT_TRUE(opened->stop());
    EXPECT_EQ(opened.events.state_events, 0U);
    EXPECT_EQ(opened.events.failed_events, 1U);
    EXPECT_EQ(opened->state(), StreamState::failed);
    EXPECT_TRUE(Rig::refused(opened->select_datacenter("dc1"),
                             StreamError::invalid_state));
  }

  template <class Configuration>
  static void is_logged_without_credentials(Configuration configuration,
                                            std::string_view route) {
    RealStream opened{std::move(configuration)};
    opened->cancel();
    EXPECT_TRUE(Rig::refused(opened->start(), StreamError::route_failed));
    EXPECT_TRUE(opened->stop());
    unsigned count = 0;
    bool primary = false;
    bool stopped = false;
    bool cancellation = false;
    bool begin = false;
    std::uint64_t sequence = 0;
    for (const auto& record : opened.drain_log()) {
      EXPECT_GT(record.sequence(), sequence);
      sequence = record.sequence();
      EXPECT_EQ(record.attempt(), opened.platform.log().attempt());
      const auto message = record.message();
      EXPECT_EQ(message.find("SYNTHETIC"), std::string_view::npos) << message;
      EXPECT_EQ(message.find(RealStream::entitlement), std::string_view::npos)
          << message;
      primary = primary ||
                (message.find(route) != std::string_view::npos &&
                 message.find("outcome=cancelled") != std::string_view::npos);
      stopped = stopped || message.find("session stop succeeded") !=
                               std::string_view::npos;
      cancellation =
          cancellation || message.find("session cancellation requested") !=
                              std::string_view::npos;
      begin = begin ||
              message.find("session start begin") != std::string_view::npos;
      ++count;
    }
    EXPECT_TRUE(primary);
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(cancellation);
    EXPECT_TRUE(begin);
    EXPECT_GE(count, 5U);
  }
};

TEST_F(LinkDriverCancelledStart, FailsACloudStreamRouteOnce) {
  fails_the_route_once(RealStream::cloud(), "CloudStream:");
}

TEST_F(LinkDriverCancelledStart, FailsALegacyCloudStreamRouteOnce) {
  fails_the_route_once(RealStream::cloud(CloudService::legacy_cloud_stream),
                       "LegacyCloudStream:");
}

TEST_F(LinkDriverCancelledStart, FailsALocalRouteOnce) {
  fails_the_route_once(RealStream::local(), "Local console:");
}

TEST_F(LinkDriverCancelledStart, FailsARemoteRouteOnce) {
  fails_the_route_once(RealStream::remote(), "Remote console:");
}

TEST_F(LinkDriverCancelledStart, LogsALocalRouteWithoutCredentials) {
  is_logged_without_credentials(RealStream::local(), "local");
}

TEST_F(LinkDriverCancelledStart, LogsARemoteRouteWithoutCredentials) {
  is_logged_without_credentials(RealStream::remote(), "remote");
}

TEST_F(LinkDriverCancelledStart, LogsACloudRouteWithoutCredentials) {
  is_logged_without_credentials(RealStream::cloud(), "cloud");
}

TEST_F(LinkDriverCancelledStart, LogsALegacyCloudRouteWithoutCredentials) {
  is_logged_without_credentials(
      RealStream::cloud(CloudService::legacy_cloud_stream), "psnow");
}

class RefusingHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest&,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    return libchiaking::unexpected(
        stop.stop_requested() ? libchiaking::platform::HttpError::cancelled
                              : libchiaking::platform::HttpError::disconnected);
  }

  int requests{};
};

class CloudProvisioning : public ::testing::Test {
 protected:
  libchiaking::platform::DefaultPlatform assembled_;
  RefusingHttp http_;
  libchiaking::platform::DefaultPlatformFactory platform_{
      libchiaking::platform::PlatformProviders{.crypto = &assembled_.crypto(),
                                               .http = &http_}};
};

TEST_F(CloudProvisioning, RejectsAConfigurationWithoutAPlayerBeforeAsking) {
  auto missing = RealStream::cloud();
  missing.user.reset();
  const auto rejected = CloudProvision::run(std::move(missing), platform_);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().result(), StreamError::invalid_configuration);
  EXPECT_FALSE(rejected.error().cancelled());
  EXPECT_EQ(rejected.error().route(),
            Rig::cloud_venue(CloudService::cloud_stream));
  EXPECT_EQ(http_.requests, 0);
}

TEST_F(CloudProvisioning, ObservesCancellation) {
  for (const auto service :
       {CloudService::cloud_stream, CloudService::legacy_cloud_stream}) {
    SCOPED_TRACE(testing::PrintToString(service));
    const auto stop = std::make_shared<test_support::WatchedStop>(true);
    auto configuration = RealStream::cloud(service);
    configuration.stop = libchiaking::platform::StopToken{stop};
    const auto started = LiveProviders::steady_us();
    const auto provisioned =
        CloudProvision::run(std::move(configuration), platform_);
    EXPECT_LT(Rig::since(started), 10s);
    ASSERT_FALSE(provisioned);
    EXPECT_GT(stop->asked.load(), 0U);
    const auto& failure = provisioned.error();
    EXPECT_TRUE(failure.cancelled());
    EXPECT_EQ(failure.route(), Rig::cloud_venue(service));
    EXPECT_EQ(failure.quit().reason, QuitReason::stopped);
    EXPECT_EQ(failure.sentence().find("SYNTHETIC"), std::string::npos);
  }
}

}  // namespace
}  // namespace play_test
