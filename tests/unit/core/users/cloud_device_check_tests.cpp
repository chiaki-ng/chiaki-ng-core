#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/json/json.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/users/cloud_authorisation_rig.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace users_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::users::AuthorisationRefusal;
using libchiaking::users::SignInState;
using Kind = CloudAuthorisation::Kind;
using Service = CloudAuthorisation::Service;

static_assert(!std::is_copy_constructible_v<libchiaking::psn::auth::GkCodes>);
static_assert(!std::is_default_constructible_v<AuthorisationRefusal>);
static_assert(!std::is_copy_constructible_v<libchiaking::users::DeviceId>);

class CloudDeviceCheck : public ::testing::Test {
 protected:
  static constexpr std::string_view legacy_cloud_stream_body =
      R"({"client_id":"bc6b0777-abb5-40da-92ca-e133cf18e989","scope":"kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get","redirect_uri":"https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html","response_type":"code","service_entity":"urn:service-entity:psn","duid":"000000070041008000112233445566778899aabbccddeeff"})";
  static constexpr std::string_view cloud_stream_body =
      R"({"client_id":"19ae39c4-3f88-4d11-a792-94e4f52c996d","scope":"id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get id_token:duid id_token:online_id openid psn:s2s","redirect_uri":"gaikai://local","response_type":"code","service_entity":"urn:service-entity:psn","duid":"000000070041008000112233445566778899aabbccddeeff"})";
  static constexpr std::string_view legacy_user_agent =
      "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
      "Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
      "Safari/537.36 gkApollo";
  static constexpr std::string_view cloud_stream_user_agent =
      "PlayStation Portal/6.0.0-rel.444+6a9cea6f5";

  static void is_sent_as_the_user(Service service,
                                  std::string_view user_agent) {
    CloudPlayer player;
    PreflightProvider provider;
    EXPECT_TRUE(CloudAuthorisation::preflight(provider, *player.user, service));
    EXPECT_EQ(provider.calls, 1U);
    const auto& request = provider.seen;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.url.view(),
              "https://ca.account.sony.com/api/authz/v3/oauth/authorizeCheck");
    ASSERT_EQ(request.headers.size(), 3U);
    EXPECT_EQ(request.headers[0].name, "Content-Type");
    EXPECT_EQ(request.headers[0].value.view(),
              "application/json; charset=UTF-8");
    EXPECT_EQ(request.headers[1].name, "User-Agent");
    EXPECT_EQ(request.headers[1].value.view(), user_agent);
    EXPECT_EQ(request.headers[2].name, "Cookie");
    EXPECT_EQ(request.headers[2].value.view(), "npsso=SYNTHETIC_TOKEN");
    EXPECT_EQ(request.connect_timeout, std::chrono::seconds{10});
    EXPECT_EQ(request.total_timeout, std::chrono::seconds{30});
    EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.maximum_response_bytes,
              libchiaking::json::max_document_bytes);
    EXPECT_NE(
        request.body.view().find(
            R"("duid":"000000070041008000112233445566778899aabbccddeeff")"),
        std::string_view::npos);
  }
};

TEST_F(CloudDeviceCheck, NamesTheUsersDeviceForEachService) {
  CloudPlayer player;
  PreflightProvider now;
  EXPECT_TRUE(CloudAuthorisation::preflight(now, *player.user,
                                            Service::legacy_cloud_stream));
  EXPECT_EQ(now.seen.body.view(), legacy_cloud_stream_body);
  PreflightProvider hosted;
  EXPECT_TRUE(CloudAuthorisation::preflight(hosted, *player.user,
                                            Service::cloud_stream));
  EXPECT_EQ(hosted.seen.body.view(), cloud_stream_body);
}

TEST_F(CloudDeviceCheck, SendsADeviceSpelledInCapitalsInLowerCase) {
  for (const auto invalid :
       {CloudPlayer::duid.substr(0, 47),
        std::string_view{"000000070041008100112233445566778899aabbccddeeff"},
        std::string_view{"0000000700410080g0112233445566778899aabbccddeeff"},
        std::string_view{}})
    EXPECT_FALSE(libchiaking::users::DeviceId::parse(invalid)) << invalid;
  constexpr std::string_view upper_text =
      "0000000700410080AB112233445566778899aabbccddeeff";
  const auto upper = libchiaking::users::DeviceId::parse(upper_text);
  ASSERT_TRUE(upper);
  EXPECT_EQ(upper->view(), "0000000700410080ab112233445566778899aabbccddeeff");
  CloudPlayer shouting{"SYNTHETIC_TOKEN", upper_text};
  PreflightProvider lowered;
  EXPECT_TRUE(CloudAuthorisation::preflight(lowered, *shouting.user,
                                            Service::cloud_stream));
  EXPECT_NE(lowered.seen.body.view().find(
                R"("duid":"0000000700410080ab112233445566778899aabbccddeeff")"),
            std::string_view::npos);
}

TEST_F(CloudDeviceCheck, ReadsTheUsersDevice) {
  CloudPlayer player;
  const auto device =
      libchiaking::users::PlayCalls{*player.user}.cloud_device();
  ASSERT_TRUE(device);
  EXPECT_EQ(device.value().view(), CloudPlayer::duid);
}

TEST_F(CloudDeviceCheck, PassesOnASuccessStatus) {
  for (const auto status : {200, 204, 299}) {
    CloudPlayer player;
    PreflightProvider provider;
    provider.status = static_cast<std::uint16_t>(status);
    EXPECT_TRUE(CloudAuthorisation::preflight(provider, *player.user,
                                              Service::legacy_cloud_stream))
        << status;
    EXPECT_NE(player.user->sign_in_state(), SignInState::expired) << status;
  }
}

TEST_F(CloudDeviceCheck, RejectsAnyOtherStatusAndExpiresAllButAServerError) {
  for (const auto status : {199, 300, 401, 500}) {
    CloudPlayer player;
    PreflightProvider provider;
    provider.status = static_cast<std::uint16_t>(status);
    const auto result = CloudAuthorisation::preflight(
        provider, *player.user, Service::legacy_cloud_stream);
    ASSERT_TRUE(CloudAuthorisation::refused_as(
        result, Kind::rejected, static_cast<std::uint16_t>(status)))
        << status;
    EXPECT_TRUE(result.error().authorisation()->denied()) << status;
    EXPECT_EQ(result.error().expires_sign_in(), status != 500) << status;
    EXPECT_EQ(player.user->is_expired(), status != 500) << status;
  }
}

TEST_F(CloudDeviceCheck,
       ReportsCancellationAndTransportFailuresWithoutExpiring) {
  CloudPlayer player;
  PreflightProvider cancelled;
  cancelled.error = platform::HttpError::cancelled;
  EXPECT_TRUE(CloudAuthorisation::refused_as(
      CloudAuthorisation::preflight(cancelled, *player.user,
                                    Service::legacy_cloud_stream),
      Kind::cancelled));
  for (const auto error : {platform::HttpError::timed_out,
                           platform::HttpError::tls_trust_failed}) {
    PreflightProvider failed;
    failed.error = error;
    EXPECT_TRUE(CloudAuthorisation::refused_as(
        CloudAuthorisation::preflight(failed, *player.user,
                                      Service::legacy_cloud_stream),
        Kind::transport));
  }
  EXPECT_FALSE(player.user->is_expired());
}

TEST_F(CloudDeviceCheck, IsSentAsTheUserForTheLegacyService) {
  is_sent_as_the_user(Service::legacy_cloud_stream, legacy_user_agent);
}

TEST_F(CloudDeviceCheck, IsSentAsTheUserForTheCloudService) {
  is_sent_as_the_user(Service::cloud_stream, cloud_stream_user_agent);
}

TEST_F(CloudDeviceCheck, NeverAdmitsAnUnusableSeed) {
  for (const auto invalid : {std::string_view{}, std::string_view{"BAD TOKEN"},
                             std::string_view{"BAD\nTOKEN"}})
    EXPECT_TRUE(CloudAuthorisation::cannot_be_admitted(invalid));
  EXPECT_TRUE(CloudAuthorisation::cannot_be_admitted(std::string(4097, 'A')));
}

TEST_F(CloudDeviceCheck, RefusesAnUnknownServiceBeforeSending) {
  CloudPlayer player;
  PreflightProvider provider;
  const auto invalid_service = CloudAuthorisation::preflight(
      provider, *player.user, static_cast<Service>(99));
  ASSERT_TRUE(
      CloudAuthorisation::refused_as(invalid_service, Kind::invalid_request));
  EXPECT_FALSE(invalid_service.error().expires_sign_in());
  EXPECT_FALSE(player.user->is_expired());
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudDeviceCheck, FollowsTheCallersStop) {
  const platform::StopToken running;
  const platform::StopToken stopped{std::make_shared<test_support::Stopped>()};
  CloudPlayer player;
  PreflightProvider provider;
  auto checked = CloudAuthorisation::preflight(
      provider, *player.user, Service::legacy_cloud_stream, running);
  EXPECT_TRUE(checked);
  EXPECT_EQ(provider.calls, 1U);
  provider.status = 401;
  checked = CloudAuthorisation::preflight(
      provider, *player.user, Service::legacy_cloud_stream, running);
  EXPECT_TRUE(CloudAuthorisation::refused_as(checked, Kind::rejected, 401));
  EXPECT_EQ(provider.calls, 2U);
  provider.error = platform::HttpError::backend_failure;
  checked = CloudAuthorisation::preflight(
      provider, *player.user, Service::legacy_cloud_stream, running);
  EXPECT_TRUE(CloudAuthorisation::refused_as(checked, Kind::transport));
  EXPECT_EQ(provider.calls, 3U);
  provider.error.reset();
  checked = CloudAuthorisation::preflight(
      provider, *player.user, Service::legacy_cloud_stream, stopped);
  EXPECT_TRUE(CloudAuthorisation::refused_as(checked, Kind::cancelled));
  EXPECT_EQ(provider.calls, 3U);
}

TEST_F(CloudDeviceCheck, SharesOneRequestBetweenIdenticalChecksInFlight) {
  CloudPlayer player;
  PreflightProvider provider;
  const auto watched = std::make_shared<test_support::WatchedStop>();
  std::atomic<bool> started{};
  provider.during = [&] {
    started = true;
    while (watched->asked == 0) Runtime::nap(std::chrono::milliseconds{1});
  };
  std::optional<CloudAuthorisation::Checked> first;
  auto leader =
      platform::Thread::start(
          Runtime::threading(), "test", platform::ThreadRole::unspecified,
          [&] {
            first.emplace(CloudAuthorisation::preflight(provider, *player.user,
                                                        Service::cloud_stream));
          })
          .value();
  while (!started) Runtime::nap(std::chrono::milliseconds{1});
  const auto second = CloudAuthorisation::preflight(
      provider, *player.user, Service::cloud_stream,
      platform::StopToken{watched});
  static_cast<void>(leader.join());
  ASSERT_TRUE(first);
  EXPECT_TRUE(*first);
  EXPECT_TRUE(second);
  EXPECT_EQ(provider.calls, 1U);
  provider.during = nullptr;
  EXPECT_TRUE(CloudAuthorisation::preflight(provider, *player.user,
                                            Service::cloud_stream));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_TRUE(CloudAuthorisation::preflight(provider, *player.user,
                                            Service::legacy_cloud_stream));
  EXPECT_EQ(provider.calls, 3U);
}

}  // namespace
}  // namespace users_test
