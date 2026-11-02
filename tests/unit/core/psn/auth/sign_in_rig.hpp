#pragma once

#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <libchiaking/users_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {

class SignInRig : public ClientRig {
 public:
  using SignInError = libchiaking::psn::auth::SignInError;
  using SignInErrorKind = libchiaking::psn::auth::SignInErrorKind;
  using RemotePlayTokens = libchiaking::psn::auth::RemotePlayTokens;
  using AppTokens = libchiaking::psn::auth::AppTokens;

  static constexpr std::string_view duid =
      "000000070041008012345678901234567890123456789012";
  static constexpr std::string_view correlation =
      "12345678-1234-1234-1234-123456789012";
  static constexpr std::string_view npsso_value = "synthetic-npsso";
  static constexpr std::string_view browser_agent =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
  static constexpr std::string_view form_content_type =
      "application/x-www-form-urlencoded";
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view legacy_token_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
  static constexpr std::string_view account_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
      "remote-access";
  static constexpr std::string_view remote_authorize_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize"
      "?client_id=ba495a24-818c-472b-b12d-ff231c1b5745"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp%20referenceDataService%3AcountryConfig.read"
      "%20pushNotification%3AwebSocket.desktop.connect%20sessionManager%"
      "3AremotePlaySession.system.update"
      "&response_type=code&access_type=offline&service_entity=urn%3Aservice-"
      "entity%"
      "3Apsn"
      "&duid=000000070041008012345678901234567890123456789012"
      "&smcid=remoteplay&layout_type=popup&PlatformPrivacyWs1=minimal&no_"
      "captcha="
      "true"
      "&cid=12345678-1234-1234-1234-123456789012";
  static constexpr std::string_view app_sso_body =
      "token_format=jwt&grant_type=sso_token&npsso=synthetic-npsso"
      "&scope=psn%3Amobile.v2.core+psn%3Aclientapp"
      "&service_entity=urn%3Aservice-entity%3Apsn"
      "&duid=00000007000801a8123456789012345678901234567890121234567890123456"
      "789012345678901212345678901234567890123456789012"
      "1234567890";
  static constexpr std::string_view remote_basic =
      "Basic "
      "YmE0OTVhMjQtODE4Yy00NzJiLWIxMmQtZmYyMzFjMWI1NzQ1Om12YWlaa1JzQXNJMUlCa1"
      "k=";
  static constexpr std::string_view app_basic =
      "Basic "
      "MDk1MTUxNTktNzIzNy00MzcwLTliNDAtMzgwNmU2N2MwODkxOnVjUGprYTV0bnRCMktxc1"
      "A=";
  static constexpr std::string_view remote_redirect =
      "https://remoteplay.dl.playstation.net/remoteplay/"
      "redirect?code=remote%2Bcode";
  static constexpr std::string_view remote_tokens_json =
      R"({"access_token":"remote-access","refresh_token":"remote-refresh","expires_in":3600,"token_type":"bearer"})";
  static constexpr std::string_view app_tokens_json =
      R"({"access_token":"mobile-access","refresh_token":"mobile-refresh","expires_in":7200})";
  static constexpr std::string_view account_json =
      R"({"user_id":"578437695752307201","online_id":"Tokyo Player"})";

  static auto npsso() -> libchiaking::psn::auth::Npsso {
    return libchiaking::psn::auth::Npsso{npsso_value};
  }

  static auto json(std::string_view body) -> Reply {
    return Reply{200, std::string{body}};
  }

  static auto header_names(const test_support::psn::Seen& seen)
      -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto& header : seen.headers) names.push_back(header.name);
    return names;
  }

  static auto within_limits(const test_support::psn::Seen& seen)
      -> testing::AssertionResult {
    if (seen.maximum_response_bytes != 1024U * 1024U)
      return testing::AssertionFailure()
             << seen.url << " allows " << seen.maximum_response_bytes
             << " bytes";
    if (seen.total_timeout != std::chrono::seconds{30})
      return testing::AssertionFailure()
             << seen.url << " waits " << seen.total_timeout.count()
             << " ms in total";
    if (seen.connect_timeout != std::chrono::seconds{10})
      return testing::AssertionFailure()
             << seen.url << " waits " << seen.connect_timeout.count()
             << " ms to connect";
    return testing::AssertionSuccess();
  }

  template <class Value>
  static auto kind_of(const libchiaking::Expected<Value, SignInError>& result)
      -> std::optional<SignInErrorKind> {
    if (result) return std::nullopt;
    return result.error().kind();
  }

  void reset() {
    http.rules.clear();
    http.seen.clear();
  }

  auto remote_authorize_outcome(Reply reply)
      -> libchiaking::Expected<RemotePlayTokens, SignInError> {
    reset();
    http.on("GET", std::string{remote_authorize_url}, {std::move(reply)});
    http.on("POST", std::string{token_url}, {json(remote_tokens_json)});
    libchiaking::psn::auth::SignIn fresh{http, steady_clock()};
    auto result = fresh.remote_play_tokens(npsso(), duid, correlation);
    EXPECT_EQ(http.seen.size(), result ? 2U : 1U);
    return result;
  }

  auto remote_refresh_outcome(Reply reply)
      -> libchiaking::Expected<RemotePlayTokens, SignInError> {
    reset();
    http.on("POST", std::string{legacy_token_url}, {std::move(reply)});
    libchiaking::psn::auth::SignIn fresh{http, steady_clock()};
    return fresh.refresh(
        libchiaking::psn::auth::RemotePlayRefreshToken{"synthetic"});
  }

  auto app_refresh_outcome(Reply reply)
      -> libchiaking::Expected<AppTokens, SignInError> {
    reset();
    http.on("POST", std::string{token_url}, {std::move(reply)});
    libchiaking::psn::auth::SignIn fresh{http, steady_clock()};
    return fresh.refresh(libchiaking::psn::auth::AppRefreshToken{"synthetic"});
  }

  libchiaking::psn::auth::SignIn sign_in{http, steady_clock(), stop_token,
                                         &log};
};

}  // namespace psn_test
