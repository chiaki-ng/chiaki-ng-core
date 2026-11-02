#pragma once

#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {

class ReplyHeader {
 public:
  std::string name;
  std::string value;
};

class ScriptedExchange {
 public:
  libchiaking::platform::HttpMethod method{
      libchiaking::platform::HttpMethod::get};
  std::string path;
  std::uint16_t status{200};
  std::string body;
  std::vector<ReplyHeader> headers;
  std::function<void(const libchiaking::platform::HttpRequest&)> check;
};

class ScriptedPeer final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& token)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    EXPECT_FALSE(token.stop_requested());
    urls.emplace_back(request.url.view());
    std::string line = std::to_string(static_cast<int>(request.method)) + " " +
                       urls.back() + "\n";
    for (const auto& field : request.headers)
      line += field.name + ": " + std::string{field.value.view()} + "\n";
    line += request.body.view();
    wire.push_back(std::move(line));
    EXPECT_LT(calls, steps.size())
        << "no exchange is scripted for " << request.url.view();
    if (calls >= steps.size())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::backend_failure);
    const auto& step = steps[calls++];
    EXPECT_EQ(request.method, step.method);
    EXPECT_NE(request.url.view().find(step.path), std::string_view::npos)
        << request.url.view();
    EXPECT_EQ(request.redirect_policy,
              libchiaking::platform::HttpRedirectPolicy::deny);
    if (step.check) step.check(request);
    if (fail_at && calls - 1 == *fail_at)
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::backend_failure);
    if (cancel_after && calls - 1 == *cancel_after) stop->stopped = true;
    libchiaking::platform::HttpResponse reply;
    reply.status_code = step.status;
    for (const auto& field : step.headers)
      reply.headers.push_back(
          {field.name, libchiaking::SecretString{field.value}});
    reply.body.assign(std::string_view{step.body});
    return reply;
  }

  std::vector<ScriptedExchange> steps;
  std::size_t calls{};
  std::shared_ptr<test_support::StopState> stop{
      std::make_shared<test_support::StopState>()};
  std::optional<std::size_t> cancel_after;
  std::optional<std::size_t> fail_at;
  std::vector<std::string> urls;
  std::vector<std::string> wire;
};

class OldServiceRequest {
 public:
  libchiaking::cloud::CloudPlatform cloud_platform{
      libchiaking::cloud::CloudPlatform::ps4};
  std::string entitlement_id;
  std::string game_identifier;
  std::string store_country{"GB"};
  std::string store_language{"en"};
  bool foreign_catalog{};
  std::shared_ptr<libchiaking::users::PsUser> user;
  libchiaking::platform::StopToken stop;
};

class OldServicePlayer {
 public:
  static constexpr std::string_view duid =
      "0000000700410080000102030405060708090a0b0c0d0e0f";

  auto session() const -> libchiaking::psn::Session& {
    return libchiaking::users::PsUser::Core::of(*user).session();
  }

  test_support::play::SignedInPlayer signed_in{
      test_support::play::SignedInPlayer::saved("SYNTHETIC", duid)};
  std::shared_ptr<libchiaking::users::PsUser> user{signed_in.user};
};

class OldServiceRig {
 public:
  using Method = libchiaking::platform::HttpMethod;
  using Request = libchiaking::platform::HttpRequest;
  using Check = std::function<void(const Request&)>;
  using Resolved =
      libchiaking::Expected<libchiaking::play::OldServiceTitle,
                            libchiaking::play::OldServiceLaunch::Error>;

  static constexpr std::string_view duid = OldServicePlayer::duid;
  static constexpr std::string_view product = "EP0000-CUSA12345_00-TITLE";
  static constexpr std::string_view container =
      R"({"default_sku":{"id":"SKU-A","entitlements":[{"id":"EP0000-CUSA12345_00-STREAM","license_type":4,"packageType":"PS4GS"}]},"playable_platform":["PS4"]})";
  static constexpr std::string_view success =
      R"({"header":{"status_code":"0x0000"}})";
  static constexpr std::string_view stream_entitlement =
      "EP0000-CUSA12345_00-STREAM";

  static auto text(const Request& request) -> std::string_view {
    return request.body.view();
  }

  static auto header(const Request& request, std::string_view name)
      -> std::string_view {
    for (const auto& field : request.headers)
      if (field.name == name) return field.value.view();
    return {};
  }

  static auto oauth(std::string_view key, std::string_view value,
                    std::string_view marker = "response_type=code")
      -> ScriptedExchange {
    return {
        Method::get,
        std::string{marker},
        302,
        {},
        {{"Location",
          (key == "code" ? std::string{libchiaking::psn::
                                           legacy_cloud_stream_redirect_uri} +
                               "?"
                         : std::string{"https://psnow.playstation.com/#"}) +
              std::string{key} + "=" + std::string{value}}},
        [](const Request& request) {
          EXPECT_EQ(header(request, "Cookie"), "npsso=SYNTHETIC");
          EXPECT_EQ(header(request, "User-Agent"),
                    libchiaking::psn::legacy_cloud_stream_http_user_agent);
          EXPECT_NE(request.url.view().find(duid), std::string_view::npos);
        }};
  }

  static auto session(bool anonymous) -> ScriptedExchange {
    std::vector<ReplyHeader> headers;
    if (anonymous)
      headers.push_back({"Set-Cookie", "JSESSIONID=FIRST; Path=/"});
    return {
        Method::post,
        "/user/session",
        200,
        std::string{success},
        std::move(headers),
        [anonymous](const Request& request) {
          EXPECT_EQ(
              text(request),
              std::string{"code="} + (anonymous ? "ANON" : "AUTH") +
                  "&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989&duid=" +
                  std::string{duid});
          EXPECT_EQ(header(request, "Content-Type"),
                    "text/plain;charset=UTF-8");
          EXPECT_EQ(header(request, "Origin"), "https://psnow.playstation.com");
          EXPECT_TRUE(
              header(request, "X-Alt-Referer").ends_with("/grc-response.html"))
              << header(request, "X-Alt-Referer");
        }};
  }

  static auto full_steps(bool acquire = true) -> std::vector<ScriptedExchange> {
    std::vector<ScriptedExchange> steps{
        oauth("code", "ANON"),
        session(true),
        {Method::get,
         "/container/GB/en/19/",
         200,
         std::string{container},
         {},
         {}},
        oauth("access_token", "COMMERCE", "response_type=token"),
        {Method::post,
         "/accounts/me/attributes",
         204,
         {},
         {},
         [](const Request& request) {
           EXPECT_EQ(header(request, "Authorization"), "Bearer COMMERCE");
           EXPECT_NE(text(request).find("PRIVACY_SETTING_BROADCAST"),
                     std::string_view::npos);
         }},
        {Method::get,
         "/internal_entitlements/"
         "EP0000-CUSA12345_00-STREAM?fields=game_meta%2Csku_meta",
         static_cast<std::uint16_t>(acquire ? 404 : 200),
         {},
         {},
         {}}};
    if (acquire) {
      steps.push_back(
          {Method::post,
           "/checkout/buynow/preview",
           200,
           R"({"header":{"status_code":"0x0000"},"data":{"cart":{"total_price_value":0,"items":[{"sku_id":"SKU-SERVER"}]}}})",
           {{"Set-Cookie", "JSESSIONID=REFRESHED; Path=/"}},
           [](const Request& request) {
             EXPECT_EQ(text(request), "sku=SKU-A");
             EXPECT_EQ(header(request, "Cookie"), "JSESSIONID=FIRST");
           }});
      steps.push_back(
          {Method::post,
           "/checkout/buynow",
           200,
           std::string{success},
           {},
           [](const Request& request) {
             EXPECT_EQ(text(request), "sku=SKU-SERVER&skipEmail=true");
             EXPECT_EQ(header(request, "Cookie"), "JSESSIONID=REFRESHED");
           }});
    }
    auto auth = oauth("code", "AUTH");
    auth.headers[0].value =
        "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
        "grc-response.html?code=AUTH";
    steps.push_back(std::move(auth));
    steps.push_back(session(false));
    return steps;
  }

  static auto without_token(std::vector<ScriptedExchange> steps)
      -> std::vector<ScriptedExchange> {
    steps.erase(steps.begin() + 3);
    return steps;
  }

  static auto config(ScriptedPeer& peer) -> OldServiceRequest {
    OldServiceRequest result;
    result.game_identifier = std::string{product};
    result.stop = libchiaking::platform::StopToken{peer.stop};
    return result;
  }

  static auto resolve(const OldServiceRequest& launch, ScriptedPeer& peer,
                      bool force_resolve,
                      bool skip_account_attributes_check = false,
                      libchiaking::play::LaunchReport progress = {})
      -> Resolved {
    libchiaking::platform::SteadyClock clock;
    libchiaking::net::http::Client client{peer, clock, launch.stop};
    std::optional<OldServicePlayer> alone;
    if (!launch.user) alone.emplace();
    libchiaking::play::OldServiceLaunch resolver{
        client, launch.user ? *launch.user : *alone->user, progress};
    return resolver.run({launch.game_identifier, launch.entitlement_id,
                         launch.cloud_platform, launch.store_country,
                         launch.store_language, launch.foreign_catalog,
                         skip_account_attributes_check, force_resolve});
  }

  static auto container_stating(std::string_view relevancy) -> std::string {
    std::string stated{container};
    stated.pop_back();
    return stated + R"(,"offers":{"skuRelevancy":)" + std::string{relevancy} +
           "}}";
  }

  static auto preview_body(std::string_view data) -> std::string {
    return std::string{R"({"header":{"status_code":"0x0000"},"data":)"} +
           std::string{data} + "}";
  }

  static auto requested_buynow(const ScriptedPeer& peer) -> bool {
    return std::ranges::any_of(peer.urls, [](const std::string& url) {
      return url.ends_with("/user/checkout/buynow");
    });
  }

  static auto token_count(const ScriptedPeer& peer) -> int {
    return count_of(peer, "response_type=token");
  }

  static auto code_count(const ScriptedPeer& peer) -> int {
    return count_of(peer, "response_type=code");
  }

  static auto count_of(const ScriptedPeer& peer, std::string_view marker)
      -> int {
    return static_cast<int>(
        std::ranges::count_if(peer.urls, [marker](const std::string& url) {
          return url.find(marker) != std::string::npos;
        }));
  }

  static auto bearer(std::string_view value) -> Check {
    return [expected = "Bearer " + std::string{value}](const Request& request) {
      EXPECT_EQ(header(request, "Authorization"), expected);
    };
  }
};

}  // namespace cloud_test
