#include "libchiaking/detail/users/device_id.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using IdentityDetail = gk::Error::IdentityDetail;
using Checked = libchiaking::Expected<void, gk::Error>;

class PreflightProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    seen = request.clone();
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{204};
  unsigned calls{};
  platform::HttpRequest seen;
};

class AuthorizeCheckPreflight : public ::testing::Test {
 protected:
  static constexpr std::string_view duid =
      "000000070041008000112233445566778899aabbccddeeff";
  static constexpr std::string_view token = "SYNTHETIC_TOKEN";
  static constexpr std::string_view legacy_agent =
      "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
      "Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 "
      "Safari/537.36 gkApollo";

  auto preflight(PreflightProvider& provider, CloudService service,
                 std::string_view npsso = token, std::string_view device = duid,
                 const platform::StopToken& stop = platform::StopToken{})
      -> Checked {
    libchiaking::net::http::Client client{provider, clock, stop};
    const auto parsed = libchiaking::users::DeviceId::parse(device);
    EXPECT_TRUE(parsed);
    test_support::play::SignedInPlayer player{
        test_support::play::SignedInPlayer::saved(npsso, device)};
    libchiaking::users::PlayCalls calls{*player.user};
    const auto checked = libchiaking::play::CloudLaunch::Core::device_checked(
        calls, client, service);
    if (!checked) return libchiaking::unexpected(checked.error());
    if (parsed) {
      EXPECT_EQ(checked.value().view(), parsed->view());
    }
    return {};
  }

  auto raw_text_refused(PreflightProvider& provider, CloudService service,
                        std::string_view device) -> testing::AssertionResult {
    libchiaking::net::http::Client client{provider, clock,
                                          platform::StopToken{}};
    libchiaking::psn::auth::Authenticator auth{
        client, libchiaking::psn::auth::Npsso{token}};
    const auto raw = auth.authorize_check(service, device);
    if (raw) return testing::AssertionFailure() << "the check passed";
    const gk::Error mapped{gk::Error::Stage::identity, raw.error()};
    if (!mapped.is_invalid_request())
      return testing::AssertionFailure() << "not an invalid request";
    if (mapped.detail() !=
        static_cast<std::uint32_t>(IdentityDetail::invalid_request))
      return testing::AssertionFailure() << "detail " << mapped.detail();
    return testing::AssertionSuccess();
  }

  static auto admissible(std::string_view npsso) -> bool {
    return test_support::play::SignedInPlayer::admissible(
        test_support::play::SignedInPlayer::saved(npsso, duid));
  }

  static auto failed_at_identity(const Checked& result, IdentityDetail detail)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the check passed";
    if (result.error().stage() != gk::Error::Stage::identity)
      return testing::AssertionFailure() << "failed outside the identity stage";
    if (result.error().detail() != static_cast<std::uint32_t>(detail))
      return testing::AssertionFailure()
             << "detail " << result.error().detail();
    return testing::AssertionSuccess();
  }

  static auto is_invalid_request(const Checked& result)
      -> testing::AssertionResult {
    auto at_identity =
        failed_at_identity(result, IdentityDetail::invalid_request);
    if (!at_identity) return at_identity;
    if (!result.error().is_invalid_request())
      return testing::AssertionFailure() << "not an invalid request";
    return testing::AssertionSuccess();
  }

  static auto is_rejected(const Checked& result) -> testing::AssertionResult {
    auto at_identity = failed_at_identity(result, IdentityDetail::rejected);
    if (!at_identity) return at_identity;
    if (result.error().reason() != gk::Error::Reason::rejected)
      return testing::AssertionFailure() << "not a rejection";
    return testing::AssertionSuccess();
  }

  static auto is_transport_failure(const Checked& result)
      -> testing::AssertionResult {
    auto at_identity =
        failed_at_identity(result, IdentityDetail::transport_failure);
    if (!at_identity) return at_identity;
    if (!result.error().is_transport())
      return testing::AssertionFailure() << "not a transport failure";
    return testing::AssertionSuccess();
  }

  static auto is_cancelled(const Checked& result) -> testing::AssertionResult {
    auto at_identity = failed_at_identity(result, IdentityDetail::cancelled);
    if (!at_identity) return at_identity;
    if (!result.error().is_cancelled())
      return testing::AssertionFailure() << "not a cancellation";
    return testing::AssertionSuccess();
  }

  void expect_request(CloudService service, std::string_view user_agent) {
    PreflightProvider provider;
    EXPECT_TRUE(preflight(provider, service));
    ASSERT_EQ(provider.calls, 1U);
    const auto& request = provider.seen;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.url.view(),
              "https://ca.account.sony.com/api/authz/v3/oauth/authorizeCheck");
    ASSERT_EQ(request.headers.size(), std::size_t{3});
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
    EXPECT_EQ(request.maximum_response_bytes, gk::Client::body_limit);
    EXPECT_NE(
        request.body.view().find(
            R"("duid":"000000070041008000112233445566778899aabbccddeeff")"),
        std::string_view::npos);
  }

  platform::SteadyClock clock;
};

using AuthorizeCheckCodec = AuthorizeCheckPreflight;
using AuthorizeCheckResult = AuthorizeCheckPreflight;
using AuthorizeCheckRequest = AuthorizeCheckPreflight;
using AuthorizeCheckClient = AuthorizeCheckPreflight;

TEST_F(AuthorizeCheckCodec, SendsTheLegacyCloudStreamBody) {
  PreflightProvider provider;
  EXPECT_TRUE(preflight(provider, CloudService::legacy_cloud_stream));
  EXPECT_EQ(
      provider.seen.body.view(),
      R"({"client_id":"bc6b0777-abb5-40da-92ca-e133cf18e989","scope":"kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get","redirect_uri":"https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html","response_type":"code","service_entity":"urn:service-entity:psn","duid":"000000070041008000112233445566778899aabbccddeeff"})");
}

TEST_F(AuthorizeCheckCodec, SendsTheCloudStreamBody) {
  PreflightProvider provider;
  EXPECT_TRUE(preflight(provider, CloudService::cloud_stream));
  EXPECT_EQ(
      provider.seen.body.view(),
      R"({"client_id":"19ae39c4-3f88-4d11-a792-94e4f52c996d","scope":"id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get id_token:duid id_token:online_id openid psn:s2s","redirect_uri":"gaikai://local","response_type":"code","service_entity":"urn:service-entity:psn","duid":"000000070041008000112233445566778899aabbccddeeff"})");
}

TEST_F(AuthorizeCheckCodec, RefusesAnInvalidDuidBeforeSending) {
  PreflightProvider rejected;
  for (const auto invalid :
       {duid.substr(0, 47),
        std::string_view{"000000070041008100112233445566778899aabbccddeeff"},
        std::string_view{"0000000700410080g0112233445566778899aabbccddeeff"}}) {
    SCOPED_TRACE(invalid);
    EXPECT_FALSE(libchiaking::users::DeviceId::parse(invalid));
    EXPECT_TRUE(
        raw_text_refused(rejected, CloudService::cloud_stream, invalid));
  }
  EXPECT_EQ(rejected.calls, 0U);
}

TEST_F(AuthorizeCheckCodec, LowercasesAParsedDuidButRefusesTheRawUppercase) {
  PreflightProvider rejected;
  constexpr std::string_view upper_text =
      "0000000700410080AB112233445566778899aabbccddeeff";
  const auto upper = libchiaking::users::DeviceId::parse(upper_text);
  ASSERT_TRUE(upper);
  EXPECT_EQ(upper->view(), "0000000700410080ab112233445566778899aabbccddeeff");
  EXPECT_TRUE(
      raw_text_refused(rejected, CloudService::cloud_stream, upper_text));
  EXPECT_EQ(rejected.calls, 0U);
}

TEST_F(AuthorizeCheckResult, PassesOnAnySuccessStatus) {
  for (const auto status : {200, 204, 299}) {
    SCOPED_TRACE(status);
    PreflightProvider provider;
    provider.status = static_cast<std::uint16_t>(status);
    EXPECT_TRUE(preflight(provider, CloudService::legacy_cloud_stream));
  }
}

TEST_F(AuthorizeCheckResult, IsRejectedByAnyOtherStatus) {
  for (const auto status : {199, 300, 401, 500}) {
    SCOPED_TRACE(status);
    PreflightProvider provider;
    provider.status = static_cast<std::uint16_t>(status);
    EXPECT_TRUE(
        is_rejected(preflight(provider, CloudService::legacy_cloud_stream)));
  }
}

TEST_F(AuthorizeCheckResult, IsCancelledWithTheRequest) {
  PreflightProvider cancelled;
  cancelled.error = platform::HttpError::cancelled;
  EXPECT_TRUE(
      is_cancelled(preflight(cancelled, CloudService::legacy_cloud_stream)));
}

TEST_F(AuthorizeCheckResult, ReportsATimeoutOrUntrustedTlsAsTransport) {
  for (const auto error : {platform::HttpError::timed_out,
                           platform::HttpError::tls_trust_failed}) {
    SCOPED_TRACE(testing::PrintToString(error));
    PreflightProvider failed;
    failed.error = error;
    EXPECT_TRUE(is_transport_failure(
        preflight(failed, CloudService::legacy_cloud_stream)));
  }
}

TEST_F(AuthorizeCheckRequest, PostsAsTheDesktopApp) {
  expect_request(CloudService::legacy_cloud_stream, legacy_agent);
}

TEST_F(AuthorizeCheckRequest, PostsAsTheCloudStreamAgent) {
  expect_request(CloudService::cloud_stream,
                 "PlayStation Portal/6.0.0-rel.444+6a9cea6f5");
}

TEST_F(AuthorizeCheckRequest, AdmitsOnlyAWellFormedNpsso) {
  EXPECT_TRUE(admissible(token));
  EXPECT_FALSE(admissible(std::string_view{}));
  EXPECT_FALSE(admissible("BAD TOKEN"));
  EXPECT_FALSE(admissible("BAD\nTOKEN"));
  EXPECT_FALSE(admissible(std::string(4097, 'A')));
}

TEST_F(AuthorizeCheckRequest, RefusesAnEmptyDuidOrUnknownServiceBeforeSending) {
  PreflightProvider provider;
  EXPECT_FALSE(libchiaking::users::DeviceId::parse(""));
  EXPECT_TRUE(
      raw_text_refused(provider, CloudService::legacy_cloud_stream, ""));
  EXPECT_TRUE(
      is_invalid_request(preflight(provider, static_cast<CloudService>(99))));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(AuthorizeCheckClient, ChecksTheDeviceInOneRequest) {
  PreflightProvider provider;
  EXPECT_TRUE(preflight(provider, CloudService::legacy_cloud_stream));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(AuthorizeCheckClient, IsRejectedByAnErrorStatus) {
  PreflightProvider provider;
  provider.status = 401;
  EXPECT_TRUE(
      is_rejected(preflight(provider, CloudService::legacy_cloud_stream)));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(AuthorizeCheckClient, ReportsABackendFailureAsTransport) {
  PreflightProvider provider;
  provider.error = platform::HttpError::backend_failure;
  EXPECT_TRUE(is_transport_failure(
      preflight(provider, CloudService::legacy_cloud_stream)));
  EXPECT_EQ(provider.calls, 1U);
}

TEST_F(AuthorizeCheckClient, SendsNothingOnceStopped) {
  PreflightProvider provider;
  const platform::StopToken stopped{std::make_shared<test_support::Stopped>()};
  EXPECT_TRUE(is_cancelled(preflight(
      provider, CloudService::legacy_cloud_stream, token, duid, stopped)));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(AuthorizeCheckClient, SendsNothingForAnInadmissibleNpsso) {
  PreflightProvider provider;
  EXPECT_FALSE(admissible("BAD TOKEN"));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(AuthorizeCheckClient, RefusesASignedOutUserBeforeSending) {
  PreflightProvider provider;
  test_support::play::SignedInPlayer left;
  EXPECT_TRUE(left.signed_in.users.sign_out(left.user->account_id()));
  libchiaking::net::http::Client client{provider, clock, platform::StopToken{}};
  libchiaking::users::PlayCalls gone{*left.user};
  const auto signed_out = libchiaking::play::CloudLaunch::Core::device_checked(
      gone, client, CloudService::legacy_cloud_stream);
  ASSERT_FALSE(signed_out);
  EXPECT_EQ(signed_out.error().stage(), gk::Error::Stage::identity);
  EXPECT_TRUE(signed_out.error().is_invalid_request());
  EXPECT_EQ(signed_out.error().detail(),
            static_cast<std::uint32_t>(IdentityDetail::invalid_request));
  EXPECT_EQ(provider.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
