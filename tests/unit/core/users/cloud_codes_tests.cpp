#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/cloud_authorisation_rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace users_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::psn::auth::GkStep;
using libchiaking::users::SignInState;
using Kind = CloudAuthorisation::Kind;
using Platform = CloudAuthorisation::Platform;
using Service = CloudAuthorisation::Service;

class CloudCodes : public ::testing::Test {
 protected:
  static auto first_and_second(const CodeProvider& provider)
      -> testing::AssertionResult {
    if (provider.urls.size() != 2)
      return testing::AssertionFailure()
             << provider.urls.size() << " codes were asked";
    if (provider.urls[0].find("client_id=gk-id") == std::string::npos)
      return testing::AssertionFailure() << provider.urls[0];
    if (provider.urls[1].find("client_id=server-id") == std::string::npos)
      return testing::AssertionFailure() << provider.urls[1];
    return testing::AssertionSuccess();
  }

  const libchiaking::psn::auth::GkClientIds ids{"gk-id", "ps3-id", "server-id"};
  const libchiaking::psn::auth::GkClientIds shared_ids{"gk-id", "server-id",
                                                       "server-id"};
  const std::vector<GkStep> both{GkStep::client, GkStep::server};
  CloudPlayer player{"synthetic-token"};
  CodeProvider provider;
};

TEST_F(CloudCodes, AreGrantedClientFirstThenServer) {
  const auto granted = CloudAuthorisation::codes(provider, *player.user, ids);
  ASSERT_TRUE(granted.codes);
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(granted.codes.value().client.view(), "synthetic-1");
  EXPECT_EQ(granted.codes.value().server.view(), "synthetic-2");
  ASSERT_EQ(provider.urls.size(), 2U);
  EXPECT_NE(provider.urls[0].find("client_id=gk-id"), std::string::npos);
  EXPECT_NE(provider.urls[1].find("client_id=server-id"), std::string::npos);
  EXPECT_NE(provider.urls[0].find("duid=" + std::string{CloudPlayer::duid}),
            std::string::npos);
  EXPECT_NE(provider.urls[1].find("duid=" + std::string{CloudPlayer::duid}),
            std::string::npos);
  EXPECT_EQ(granted.steps, both);
  EXPECT_NE(player.user->sign_in_state(), SignInState::expired);
}

TEST_F(CloudCodes, StopAtAFailedClientCodeWithoutExpiring) {
  provider.fail_call = 1;
  const auto granted = CloudAuthorisation::codes(provider, *player.user, ids);
  EXPECT_TRUE(CloudAuthorisation::code_refused_as(granted, Kind::transport));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(granted.steps, std::vector<GkStep>{GkStep::client});
  EXPECT_FALSE(player.user->is_expired());
}

TEST_F(CloudCodes, ExpireTheSignInWhenTheServerCodeIsRejected) {
  provider.reject_call = 2;
  const auto granted = CloudAuthorisation::codes(provider, *player.user, ids);
  ASSERT_TRUE(
      CloudAuthorisation::code_refused_as(granted, Kind::rejected, 200));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_TRUE(granted.codes.error().authorisation()->denied());
  EXPECT_EQ(granted.steps, both);
  EXPECT_TRUE(player.user->is_expired());
}

TEST_F(CloudCodes, AreCancelledWhenTheServerCodeIsCancelled) {
  provider.cancel_call = 2;
  const auto granted = CloudAuthorisation::codes(provider, *player.user, ids);
  ASSERT_TRUE(CloudAuthorisation::code_refused_as(granted, Kind::cancelled));
  EXPECT_TRUE(granted.codes.error().is_cancelled());
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(granted.steps, both);
  EXPECT_FALSE(player.user->is_expired());
}

TEST_F(CloudCodes, RefuseAnInvalidClientIdBeforeSending) {
  auto invalid = ids;
  invalid.stream_server_client_id = "not/an/id";
  const auto granted =
      CloudAuthorisation::codes(provider, *player.user, invalid);
  EXPECT_TRUE(
      CloudAuthorisation::code_refused_as(granted, Kind::invalid_request));
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(granted.steps.empty());
  EXPECT_FALSE(player.user->is_expired());
}

TEST_F(CloudCodes, AreNeverAskedForAnUnusableDeviceOrSeed) {
  EXPECT_FALSE(libchiaking::users::DeviceId::parse("short"));
  EXPECT_TRUE(CloudAuthorisation::cannot_be_admitted("token;other=1"));
}

TEST_F(CloudCodes, AreNotAskedOnceTheCallerHasStopped) {
  const auto granted = CloudAuthorisation::codes(
      provider, *player.user, ids, Service::cloud_stream, Platform::ps5,
      platform::StopToken{std::make_shared<test_support::Stopped>()});
  ASSERT_TRUE(CloudAuthorisation::code_refused_as(granted, Kind::cancelled));
  EXPECT_TRUE(granted.codes.error().is_cancelled());
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudCodes, FollowTheCloudServiceOnThePs5) {
  const auto granted =
      CloudAuthorisation::codes(provider, *player.user, shared_ids);
  EXPECT_TRUE(granted.codes);
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_TRUE(first_and_second(provider));
  EXPECT_TRUE(provider.urls[0].starts_with(
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize?"))
      << provider.urls[0];
  EXPECT_NE(provider.urls[1].find("oauth:create_authn_ticket_for_cloud_"
                                  "console_signin"),
            std::string::npos);
}

TEST_F(CloudCodes, FollowTheLegacyServiceOnThePs4) {
  const auto granted =
      CloudAuthorisation::codes(provider, *player.user, shared_ids,
                                Service::legacy_cloud_stream, Platform::ps4);
  EXPECT_TRUE(granted.codes);
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_TRUE(first_and_second(provider));
  EXPECT_TRUE(provider.urls[0].starts_with(
      "https://ca.account.sony.com/api/v1/oauth/authorize?"))
      << provider.urls[0];
  EXPECT_NE(provider.urls[1].find("scope=sso:none&duid="), std::string::npos);
}

TEST_F(CloudCodes, FollowTheLegacyServiceOnThePs3WithoutADevice) {
  const auto granted =
      CloudAuthorisation::codes(provider, *player.user, shared_ids,
                                Service::legacy_cloud_stream, Platform::ps3);
  EXPECT_TRUE(granted.codes);
  EXPECT_EQ(provider.calls, 2U);
  ASSERT_TRUE(first_and_second(provider));
  EXPECT_NE(provider.urls[1].find("scope=kamaji:commerce_native&"),
            std::string::npos);
  EXPECT_EQ(provider.urls[1].find("duid="), std::string::npos);
}

TEST_F(CloudCodes, ReportAServerErrorWithoutExpiring) {
  provider.reject_call = 2;
  provider.reject_status = 500;
  const auto granted =
      CloudAuthorisation::codes(provider, *player.user, shared_ids,
                                Service::legacy_cloud_stream, Platform::ps4);
  ASSERT_TRUE(
      CloudAuthorisation::code_refused_as(granted, Kind::server_error, 500));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_FALSE(granted.codes.error().expires_sign_in());
  EXPECT_FALSE(player.user->is_expired());
}

TEST_F(CloudCodes, RefuseTheCloudServiceOnThePs4BeforeSending) {
  const auto granted = CloudAuthorisation::codes(
      provider, *player.user, shared_ids, Service::cloud_stream, Platform::ps4);
  EXPECT_TRUE(
      CloudAuthorisation::code_refused_as(granted, Kind::invalid_request));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(CloudCodes, AreAskedAfreshEachTime) {
  const auto first =
      CloudAuthorisation::codes(provider, *player.user, shared_ids);
  const auto second =
      CloudAuthorisation::codes(provider, *player.user, shared_ids);
  EXPECT_TRUE(first.codes);
  ASSERT_TRUE(second.codes);
  EXPECT_EQ(provider.calls, 4U);
  EXPECT_EQ(second.codes.value().client.view(), "synthetic-3");
}

TEST(CloudAuthorisationForAHomeUser, IsNeverGranted) {
  test_support::users::SignedIn signed_in;
  const auto added = signed_in.users.add_home_user(libchiaking::AccountId{90},
                                                   "Living Room Player");
  ASSERT_TRUE(added);
  auto& home = *added.value();
  PreflightProvider provider;
  EXPECT_TRUE(CloudAuthorisation::signed_out(
      libchiaking::users::PlayCalls{home}.cloud_device()));
  EXPECT_TRUE(CloudAuthorisation::signed_out(CloudAuthorisation::preflight(
      provider, home, Service::legacy_cloud_stream)));
  EXPECT_TRUE(CloudAuthorisation::signed_out(CloudAuthorisation::preflight(
      provider, home, Service::cloud_stream,
      platform::StopToken{std::make_shared<test_support::Stopped>()})));
  CodeProvider code_provider;
  const auto granted = CloudAuthorisation::codes(
      code_provider, home, {"gk-id", "ps3-id", "server-id"});
  EXPECT_TRUE(CloudAuthorisation::signed_out(granted.codes));
  EXPECT_TRUE(granted.steps.empty());
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(code_provider.calls, 0U);
  EXPECT_EQ(signed_in.offline.requests, 0);
  EXPECT_EQ(home.sign_in_state(), SignInState::signed_out);
}

}  // namespace
}  // namespace users_test
