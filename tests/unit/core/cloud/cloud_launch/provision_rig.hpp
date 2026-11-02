#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/psn/server_clock.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/cloud/session_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {

class TitleProgress final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&,
                   libchiaking::net::protocol::remote_play::FailureStage stage,
                   std::string_view text) override {
    (stage == libchiaking::net::protocol::remote_play::FailureStage::cloud_title
         ? title_texts
         : gk_texts)
        .emplace_back(text);
  }

  std::vector<std::string> title_texts;
  std::vector<std::string> gk_texts;
};

class RetryProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    platform::HttpResponse response;
    response.status_code = 200;
    std::string_view body;
    const auto url = request.url.view();
    if (url.find("authorizeCheck") != std::string::npos) {
      ++preflights;
      response.status_code = 204;
    } else if (url.find("client_id=bc6b0777-") != std::string::npos) {
      EXPECT_NE(url.find("000000070041008042424242424242424242424242424242"),
                std::string::npos);
      response.status_code = 302;
      response.headers.push_back(
          {"Location", libchiaking::SecretString{
                           "https://psnow.playstation.com/app/2.2.0/133/"
                           "5cdcc037d/grc-response.html?code=ACCOUNT"}});
    } else if (url.ends_with("/user/session")) {
      ++account_sessions;
      response.headers.push_back(
          {"Set-Cookie",
           libchiaking::SecretString{"JSESSIONID=SYNTHETIC; Path=/"}});
      body = R"({"header":{"status_code":"0x0000"}})";
    } else if (url.find("/container/") != std::string::npos) {
      ++lookups;
      body =
          R"({"default_sku":{"id":"SKU","entitlements":[{"id":"RESOLVED","license_type":4}]},"playable_platform":["PS3"]})";
    } else if (url.find("response_type=token") != std::string::npos) {
      response.status_code = 302;
      response.headers.push_back(
          {"Location",
           libchiaking::SecretString{
               "https://psnow.playstation.com/#access_token=SYNTHETIC"}});
    } else if (url.ends_with("/accounts/me/attributes")) {
      ++attribute_checks;
      response.status_code = 204;
      if (reject_attributes) {
        response.status_code = 400;
        body =
            R"({"error":{"validationErrors":[{"missingElements":[{"name":"PRIVACY_SETTING_BROADCAST"}]}]}})";
      }
    } else if (url.find("/internal_entitlements/") != std::string::npos) {
      response.status_code = 200;
    } else if (url.find("/sessions/start?") != std::string::npos) {
      ++starts;
      const auto json = request.body.view();
      const bool owned = json.find("OWNED") != std::string::npos;
      EXPECT_TRUE(owned || json.find("RESOLVED") != std::string::npos) << json;
      if (!owned && !reject_again) return gk.perform(request, stop);
      response.status_code = 400;
      body = unrelated_failure ? R"({"name":"unrelated"})"
                               : R"({"name":"noGameForEntitlementId"})";
      gk.calls = 0;
    } else {
      return gk.perform(request, stop);
    }
    for (char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  GatewayProvider gk;
  unsigned preflights{};
  unsigned lookups{};
  unsigned starts{};
  unsigned account_sessions{};
  unsigned attribute_checks{};
  bool reject_again{};
  bool unrelated_failure{};
  bool reject_attributes{};
};

class Recorded {
 public:
  bool operator==(const Recorded&) const = default;

  platform::HttpMethod method{};
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

class CheckedProvider final : public platform::HttpProvider {
 public:
  explicit CheckedProvider(platform::HttpProvider& inner) : inner_(inner) {}

  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    if (request.url.view().starts_with(
            libchiaking::psn::graphql::app_endpoint) ||
        request.url.view().starts_with(
            libchiaking::psn::graphql::web_endpoint)) {
      graphql.emplace_back(request.url.view());
      platform::HttpResponse response;
      response.status_code = 404;
      for (const auto& [fragment, body] : bodies) {
        if (request.url.view().find(fragment) == std::string::npos) continue;
        response.status_code = 200;
        for (char character : body)
          response.body.push_back(static_cast<std::byte>(character));
        break;
      }
      return response;
    }
    Recorded recorded{request.method,
                      std::string{request.url.view()},
                      {},
                      std::string{request.body.view()}};
    for (const auto& field : request.headers)
      recorded.headers.emplace_back(field.name, field.value.view());
    others.push_back(std::move(recorded));
    return inner_.perform(request, stop);
  }

  std::vector<std::pair<std::string, std::string>> bodies;
  std::vector<std::string> graphql;
  std::vector<Recorded> others;

 private:
  platform::HttpProvider& inner_;
};

class ProvisionRig {
 public:
  using Run = decltype(std::declval<play::CloudLaunch::Core&>().run());

  ProvisionRig() { waiter.answer = false; }

  auto provision_cleared(
      const test_support::play::LaunchCase& launched,
      platform::HttpProvider& http,
      test_support::play::ScriptedClearance& clearance,
      play::LaunchObserver& observer = play::LaunchObserver::silent(),
      libchiaking::psn::ServerClock* server_clock = nullptr) -> Run {
    test_support::play::RecordingListener listener{observer};
    play::CloudLaunch::Core core{
        {http, network, clock, test_support::process_threading(), random,
         waiter, clearance, launched.utc_offset, nullptr, nullptr,
         server_clock},
        launched.request(),
        listener};
    return core.run();
  }

  auto provision(
      const test_support::play::LaunchCase& launched,
      platform::HttpProvider& http,
      play::LaunchObserver& observer = play::LaunchObserver::silent()) -> Run {
    auto clearance = launched.clearance();
    return provision_cleared(launched, http, clearance, observer);
  }

  platform::SteadyClock steady;
  platform::DefaultNetworkProvider network{test_support::process_threading(),
                                           steady};
  test_support::FakeClock clock{1'000};
  test_support::FixedRandom random;
  test_support::FakeWaiter waiter;
};

}  // namespace cloud_test
