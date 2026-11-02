#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::StreamingPlatform;
using libchiaking::psn::StreamingService;
using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::GkClientIds;
using libchiaking::psn::auth::GkStep;
using Reply = ClientRig::Reply;

class GkCodes : public ::testing::Test {
 protected:
  class Recorder final : public libchiaking::platform::HttpProvider {
   public:
    auto perform(const libchiaking::platform::HttpRequest& request,
                 const libchiaking::platform::StopToken&)
        -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                                 libchiaking::platform::HttpError> override {
      seen.push_back(request.clone());
      libchiaking::platform::HttpResponse response;
      response.status_code = 302;
      const bool legacy =
          request.url.view().find("grc-response") != std::string_view::npos;
      response.headers.push_back(
          {"Location",
           libchiaking::SecretString{
               legacy ? "https://psnow.playstation.com/app/2.2.0/133/"
                        "5cdcc037d/grc-response.html?code=C"
                      : "gaikai://local?code=C"}});
      return response;
    }

    std::vector<libchiaking::platform::HttpRequest> seen;
  };

  static constexpr std::string_view cloud_stream_client_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/"
      "authorize?response_type=code"
      "&client_id=client-123&redirect_uri=gaikai://local"
      "&service_entity=urn:service-entity:psn&prompt=none"
      "&duid=000000070041008000112233445566778899aabbccddeeff"
      "&smcid=qlite&applicationId=qlite&mid=qlite"
      "&scope=id_token:psn.basic_claims%20kamaji:s2s.subscriptionsPremium.get"
      "%20id_token:duid%20id_token:online_id%20openid%20psn:s2s";
  static constexpr std::string_view cloud_stream_server_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize"
      "?response_type=code&redirect_uri=gaikai://local"
      "&service_entity=urn:service-entity:psn&prompt=none&client_id=server-123"
      "&smcid=qlite&applicationId=qlite&mid=qlite"
      "&scope=id_token:duid%20id_token:online_id%20openid%20"
      "oauth:create_authn_ticket_for_cloud_console_signin"
      "&duid=000000070041008000112233445566778899aabbccddeeff";
  static constexpr std::string_view legacy_cloud_stream_client_url =
      "https://ca.account.sony.com/api/v1/oauth/authorize?response_type=code"
      "&client_id=legacy_cloud_stream-client-1"
      "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html"
      "&service_entity=urn:service-entity:psn&prompt=none"
      "&duid=000000070041008000112233445566778899aabbccddeeff"
      "&smcid=pc:psnow&applicationId=psnow&mid=PSNOW"
      "&scope=kamaji:commerce_native%20versa:user_update_entitlements_first_"
      "play%"
      "20kamaji:lists"
      "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&"
      "displayFooter="
      "none"
      "&disableLinks=qriocityLink&layout_type=popup&service_logo=ps&tp_psn="
      "true&"
      "noEVBlock=true";
  static constexpr std::string_view legacy_cloud_stream_server_base =
      "https://ca.account.sony.com/api/v1/oauth/authorize?response_type=code"
      "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html"
      "&service_entity=urn:service-entity:psn&prompt=none&client_id=gk-client"
      "&smcid=pc:psnow&applicationId=psnow&mid=PSNOW";
  static constexpr std::string_view legacy_cloud_stream_server_suffix =
      "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink"
      "&displayFooter=none&disableLinks=qriocityLink&layout_type=popup"
      "&service_logo=ps&tp_psn=true&noEVBlock=true";

  static auto with_id(std::string_view url, std::string_view from,
                      std::string_view to) -> std::string {
    std::string result{url};
    result.replace(result.find(from), from.size(), to);
    return result;
  }

  static auto gk(StreamingService service, StreamingPlatform target,
                 const GkClientIds& ids, std::string_view duid,
                 std::vector<std::string>* urls = nullptr)
      -> libchiaking::Expected<libchiaking::psn::auth::GkCodes,
                               AuthRig::AuthError> {
    Recorder recorder;
    libchiaking::net::http::Client client{recorder, ClientRig::steady_clock(),
                                          libchiaking::platform::StopToken{}};
    libchiaking::psn::auth::Authenticator auth{
        client, libchiaking::psn::auth::Npsso{"NPSSO-FAKE"}};
    auto result = auth.gk_codes(service, target, ids, duid);
    for (const auto& request : recorder.seen) {
      EXPECT_EQ(request.method, libchiaking::platform::HttpMethod::get);
      EXPECT_EQ(request.headers.size(), 2U);
      if (request.headers.size() == 2) {
        EXPECT_EQ(request.headers[0].name, "User-Agent");
        EXPECT_EQ(request.headers[1].value.view(), "npsso=NPSSO-FAKE");
      }
      if (urls != nullptr) urls->emplace_back(request.url.view());
    }
    return result;
  }

  static auto refused(StreamingService service, StreamingPlatform target,
                      const GkClientIds& ids, std::string_view duid)
      -> testing::AssertionResult {
    std::vector<std::string> sent;
    const auto result = gk(service, target, ids, duid, &sent);
    if (result) return testing::AssertionFailure() << "the codes were issued";
    if (result.error().kind() != AuthErrorKind::invalid_request)
      return testing::AssertionFailure()
             << "refused as " << result.error().name();
    if (!sent.empty())
      return testing::AssertionFailure() << sent.size() << " requests went out";
    return testing::AssertionSuccess();
  }

  static auto steps_into(std::vector<GkStep>& steps) {
    return [&steps](GkStep step) { steps.push_back(step); };
  }

  const GkClientIds ids{"gk-id", "ps3-id", "server-id"};
};

TEST_F(GkCodes, BuildTheCloudStreamUrls) {
  std::vector<std::string> urls;
  EXPECT_TRUE(gk(StreamingService::cloud_stream, StreamingPlatform::ps5,
                 {"client-123", "unused", "server-123"}, AuthRig::gk_duid,
                 &urls));
  ASSERT_EQ(urls.size(), 2U);
  EXPECT_EQ(urls[0], cloud_stream_client_url);
  EXPECT_EQ(urls[1], cloud_stream_server_url);
}

TEST_F(GkCodes, BuildTheLegacyCloudStreamUrls) {
  const auto other_expected =
      std::string{legacy_cloud_stream_server_base} +
      "&scope=sso:none&duid=" + std::string{AuthRig::gk_duid} +
      std::string{legacy_cloud_stream_server_suffix};
  for (const auto target : {StreamingPlatform::ps4, StreamingPlatform::ps5}) {
    SCOPED_TRACE(static_cast<int>(target));
    std::vector<std::string> urls;
    EXPECT_TRUE(gk(StreamingService::legacy_cloud_stream, target,
                   {"legacy_cloud_stream-client-1", "gk-client", "unused"},
                   AuthRig::gk_duid, &urls));
    ASSERT_EQ(urls.size(), 2U);
    EXPECT_EQ(urls[0], legacy_cloud_stream_client_url);
    EXPECT_EQ(urls[1], other_expected);
  }
  std::vector<std::string> urls;
  const auto ps3_expected = std::string{legacy_cloud_stream_server_base} +
                            "&scope=kamaji:commerce_native" +
                            std::string{legacy_cloud_stream_server_suffix};
  EXPECT_TRUE(gk(StreamingService::legacy_cloud_stream, StreamingPlatform::ps3,
                 {"legacy_cloud_stream-client-1", "gk-client", "unused"},
                 AuthRig::gk_duid, &urls));
  ASSERT_EQ(urls.size(), 2U);
  EXPECT_EQ(urls[1], ps3_expected);
}

TEST_F(GkCodes, AcceptClientIdsAtTheirBounds) {
  EXPECT_TRUE(gk(StreamingService::cloud_stream, StreamingPlatform::ps5,
                 {std::string(127, 'a'), "", "server"}, AuthRig::gk_duid));
  EXPECT_TRUE(gk(StreamingService::legacy_cloud_stream, StreamingPlatform::ps4,
                 {"client._~", "server", ""}, AuthRig::gk_duid));
}

TEST_F(GkCodes, RefuseABadClientIdBeforeSending) {
  for (const auto& bad : {std::string{}, std::string{"a&b"},
                          std::string(128, 'a'), std::string{"id/with?bad"}}) {
    SCOPED_TRACE(bad);
    EXPECT_TRUE(refused(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        {bad, "", "server"}, AuthRig::gk_duid));
    EXPECT_TRUE(refused(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        {"client", "", bad}, AuthRig::gk_duid));
    EXPECT_TRUE(refused(StreamingService::legacy_cloud_stream,
                        StreamingPlatform::ps3, {"client", bad, ""},
                        AuthRig::gk_duid));
  }
}

TEST_F(GkCodes, RefuseABadDuidBeforeSending) {
  for (const auto bad :
       {std::string_view{"short"},
        std::string_view{"000000070041008100112233445566778899aabbccddeeff"},
        AuthRig::gk_duid.substr(0, 47),
        std::string_view{"000000070041008000112233445566778899aabbccddeeff0"},
        std::string_view{"000000070041008000112233445566778899aabbccddeefF"},
        std::string_view{"100000070041008000112233445566778899aabbccddeeff"}}) {
    SCOPED_TRACE(bad);
    EXPECT_TRUE(refused(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        ids, bad));
    EXPECT_TRUE(refused(StreamingService::legacy_cloud_stream,
                        StreamingPlatform::ps4, ids, bad));
  }
}

TEST_F(GkCodes, RefuseAnUnsupportedServiceOrPlatform) {
  EXPECT_TRUE(refused(StreamingService::cloud_stream, StreamingPlatform::ps4,
                      ids, AuthRig::gk_duid));
  EXPECT_TRUE(refused(StreamingService::legacy_cloud_stream,
                      static_cast<StreamingPlatform>(0xff), ids,
                      AuthRig::gk_duid));
  EXPECT_TRUE(refused(static_cast<StreamingService>(0xff),
                      StreamingPlatform::ps4, ids, AuthRig::gk_duid));
  auto invalid = ids;
  invalid.ps3_gk_client_id.clear();
  EXPECT_TRUE(refused(StreamingService::legacy_cloud_stream,
                      StreamingPlatform::ps4, invalid, AuthRig::gk_duid));
}

TEST_F(GkCodes, PickTheRightClientIds) {
  std::vector<std::string> urls;
  EXPECT_TRUE(gk(StreamingService::cloud_stream, StreamingPlatform::ps5, ids,
                 AuthRig::gk_duid, &urls));
  ASSERT_EQ(urls.size(), 2U);
  EXPECT_NE(urls[0].find("client_id=gk-id"), std::string::npos) << urls[0];
  EXPECT_NE(urls[1].find("client_id=server-id"), std::string::npos) << urls[1];
  EXPECT_NE(urls[0].find("/api/authz/v3/"), std::string::npos) << urls[0];
  for (const auto target : {StreamingPlatform::ps3, StreamingPlatform::ps4,
                            StreamingPlatform::ps5}) {
    SCOPED_TRACE(static_cast<int>(target));
    urls.clear();
    EXPECT_TRUE(gk(StreamingService::legacy_cloud_stream, target, ids,
                   AuthRig::gk_duid, &urls));
    ASSERT_EQ(urls.size(), 2U);
    EXPECT_NE(urls[0].find("client_id=gk-id"), std::string::npos) << urls[0];
    EXPECT_NE(urls[1].find("client_id=ps3-id"), std::string::npos) << urls[1];
    EXPECT_NE(urls[0].find("/api/v1/"), std::string::npos) << urls[0];
  }
}

TEST_F(GkCodes, AreFetchedClientFirst) {
  AuthRig rig;
  rig.http.on("GET", with_id(cloud_stream_client_url, "client-123", "gk-id"),
              {AuthRig::redirect("gaikai://local?code=CLIENT")});
  rig.http.on("GET",
              with_id(cloud_stream_server_url, "server-123", "server-id"),
              {AuthRig::redirect("gaikai://local?code=SERVER")});
  std::vector<GkStep> steps;
  const auto codes =
      rig.auth.gk_codes(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        ids, AuthRig::gk_duid, steps_into(steps));
  ASSERT_TRUE(codes);
  EXPECT_EQ(codes.value().client.view(), "CLIENT");
  EXPECT_EQ(codes.value().server.view(), "SERVER");
  EXPECT_EQ(steps, (std::vector<GkStep>{GkStep::client, GkStep::server}));
  ASSERT_EQ(rig.http.seen.size(), 2U);
  ASSERT_EQ(rig.http.seen[0].headers.size(), 2U);
  EXPECT_EQ(rig.http.seen[0].headers[0].name, "User-Agent");
  EXPECT_EQ(rig.http.seen[0].headers[0].value, AuthRig::cloud_stream_agent);
  EXPECT_EQ(rig.http.seen[0].headers[1].value, "npsso=NPSSO-FAKE");
}

TEST_F(GkCodes, StopAfterARefusedClientCode) {
  AuthRig rig;
  rig.http.on("GET", with_id(cloud_stream_client_url, "client-123", "gk-id"),
              {Reply{200, ""}});
  std::vector<GkStep> steps;
  const auto codes =
      rig.auth.gk_codes(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        ids, AuthRig::gk_duid, steps_into(steps));
  ASSERT_FALSE(codes);
  EXPECT_TRUE(codes.error().denied());
  EXPECT_EQ(rig.http.seen.size(), 1U);
  EXPECT_EQ(steps, (std::vector<GkStep>{GkStep::client}));
}

TEST_F(GkCodes, CheckTheRequestBeforeTheNpsso) {
  AuthRig invalid;
  std::vector<GkStep> steps;
  const auto unsupported = invalid.auth.gk_codes(
      StreamingService::cloud_stream, StreamingPlatform::ps4, ids,
      AuthRig::gk_duid, steps_into(steps));
  EXPECT_EQ(AuthRig::kind_of(unsupported), AuthErrorKind::invalid_request);
  EXPECT_TRUE(invalid.http.seen.empty());
  EXPECT_TRUE(steps.empty());

  AuthRig bad{"BAD TOKEN"};
  EXPECT_EQ(AuthRig::kind_of(bad.auth.gk_codes(StreamingService::cloud_stream,
                                               StreamingPlatform::ps4, ids,
                                               AuthRig::gk_duid)),
            AuthErrorKind::invalid_request);
  EXPECT_EQ(AuthRig::kind_of(bad.auth.gk_codes(StreamingService::cloud_stream,
                                               StreamingPlatform::ps5, ids,
                                               AuthRig::gk_duid)),
            AuthErrorKind::invalid_npsso);
  EXPECT_TRUE(bad.http.seen.empty());
}

TEST_F(GkCodes, SkipTheServerCodeAfterAStopDuringTheClientCode) {
  AuthRig rig;
  rig.http.on(
      "GET", with_id(cloud_stream_client_url, "client-123", "gk-id"),
      {rig.stopping_during(AuthRig::redirect("gaikai://local?code=CLIENT"))});
  const auto codes =
      rig.auth.gk_codes(StreamingService::cloud_stream, StreamingPlatform::ps5,
                        ids, AuthRig::gk_duid);
  EXPECT_EQ(AuthRig::kind_of(codes), AuthErrorKind::cancelled);
  EXPECT_EQ(rig.http.seen.size(), 1U);
  EXPECT_TRUE(rig.log.contains("psn.auth.gk_server_code"));
}

}  // namespace
}  // namespace psn_test
