#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/testing/in_memory_record_store.hpp"
#include "libchiaking/testing/manual_clock.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/play/scripted_clearance.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/play/cloud_provision.hpp"
#include "tests/unit/core/play/held_catalog.hpp"
#include "tests/unit/core/play/launch_transcript.hpp"
#include "tests/unit/core/play/remote_route_network_fixture.hpp"
#include "tests/unit/core/play/remote_start_script.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test::cloud_transcripts {

using test_support::FakeWaiter;
using test_support::LiveProviders;
using test_support::play::SignedInPlayer;

class TranscriptPs final : public libchiaking::platform::HttpProvider {
 public:
  using Reply = libchiaking::Expected<libchiaking::platform::HttpResponse,
                                      libchiaking::platform::HttpError>;

  static constexpr std::string_view npsso = "synthetic-token";
  static constexpr std::string_view app_token = "synthetic-app-token";
  static constexpr std::string_view device_id =
      "000000070041008000000000000000000000000000000000";
  static constexpr std::string_view session_id = "synthetic-session";
  static constexpr std::string_view handshake_key = "synthetic-handshake";

  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest& request,
                             const libchiaking::platform::StopToken&)
      -> Reply override {
    const auto url = request.url.view();
    asked.emplace_back(url);
    const auto has = [url](std::string_view part) {
      return url.find(part) != std::string_view::npos;
    };
    if (url.starts_with(libchiaking::psn::graphql::app_endpoint) ||
        url.starts_with(libchiaking::psn::graphql::web_endpoint))
      return graphql(url);
    if (has("/oauth/authorizeCheck")) return replied(204, {});
    if (has("client_id=bc6b0777-"))
      return redirected(
          "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
          "grc-response.html?code=ACCOUNT");
    if (has("response_type=token"))
      return redirected(
          "https://psnow.playstation.com/#access_token=SYNTHETIC");
    if (has("/oauth/authorize?"))
      return redirected(
          has("client_id=server-id")
              ? (request.url.view().find("grc-response") !=
                         std::string_view::npos
                     ? std::string{"https://psnow.playstation.com/app/2.2.0/"
                                   "133/5cdcc037d/grc-response.html"}
                     : std::string{"gaikai://local"}) +
                    "?code=SERVER"
              : (request.url.view().find("grc-response") !=
                         std::string_view::npos
                     ? std::string{"https://psnow.playstation.com/app/2.2.0/"
                                   "133/5cdcc037d/grc-response.html"}
                     : std::string{"gaikai://local"}) +
                    "?code=CLIENT");
    if (url.ends_with("/user/session"))
      return replied(200, R"({"header":{"status_code":"0x0000"}})",
                     {"Set-Cookie", "JSESSIONID=SYNTHETIC; Path=/"});
    if (has("/container/"))
      return replied(
          200,
          R"({"default_sku":{"id":"SKU","entitlements":[{"id":"RESOLVED","license_type":4}]},"playable_platform":["PS3"]})");
    if (url.ends_with("/accounts/me/attributes")) return replied(204, {});
    if (has("/internal_entitlements/")) return replied(200, {});
    if (has("/internal/entitlements/"))
      return replied(
          200,
          R"({"id":"EP0109-PPSA07033_00-FAKE0030TITLE000","productId":"EP0109-PPSA07033_00-FAKE0030TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true})");
    if (has("/api/catalog/v2/products/cloudgame"))
      return replied(200, R"({"productInfo":[]})");
    if (has("/api/catalog/v2/categories/83999501-dbe5-413a-b265-6484e0a7626e"))
      return replied(200, R"({"totalNumProducts":0,"products":[]})");
    if (has("/portal/np/portal-config.json"))
      return replied(200, R"({"excludedTitleIds":[]})");
    if (has("/premium/features/cloudgame"))
      return replied(
          200,
          R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1","tierLabel":"TIER_30","benefitPlanIds":["SYNTHETIC-PLAN"]}]})");
    if (has("/client_ids"))
      return replied(
          200,
          R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})");
    if (url.ends_with("/config"))
      return replied(200, R"({"configKey":"config-key"})");
    if (has("/sessions/start")) {
      if (no_game_refusals > 0) {
        --no_game_refusals;
        return replied(400, R"({"name":"noGameForEntitlementId"})");
      }
      return replied(200, R"({"sessionId":"synthetic-session"})",
                     {"X-Gaikai-Session", "start-key"});
    }
    if (url.ends_with("/authorize"))
      return replied(200, {}, {"X-Gaikai-Session", "authorize-key"});
    if (has("/lock?forceLogout=true")) {
      if (busy_locks > 0) {
        --busy_locks;
        return replied(200, R"({"lockAcquired":false,"pollFrequency":1})");
      }
      return replied(200, R"({"lockAcquired":true,"pollFrequency":5})");
    }
    if (url.ends_with("/datacenters"))
      return replied(
          200,
          R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])",
          {"X-Gaikai-Session", "datacenter-key"});
    if (url.ends_with("/datacenters/select"))
      return replied(200, R"({"port":8975})",
                     {"X-Gaikai-Session", "select-key"});
    if (url.ends_with("/allocate"))
      return replied(
          200,
          R"({"launchSlot":{"publicIp":"203.0.113.5","port":9295,"privateIp":"10.0.0.7"},"handshakeKey":"synthetic-handshake","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})",
          {"X-Gaikai-Session", "allocation-key"});
    ++unscripted;
    return replied(404, {});
  }

  void conceal_in(play_transcript::Transcript& transcript) const {
    transcript.conceal(npsso, "npsso");
    transcript.conceal(app_token, "app-token");
    transcript.conceal(device_id, "device-id");
    transcript.conceal(session_id, "session-id");
    transcript.conceal(handshake_key, "handshake-key");
    for (const std::string_view key :
         {"config-key", "start-key", "authorize-key", "datacenter-key",
          "select-key", "allocation-key"})
      transcript.conceal(key, "session-key");
    for (const std::string_view code : {"ACCOUNT", "CLIENT", "SERVER"})
      transcript.conceal(code, "auth-code");
    transcript.conceal("SYNTHETIC", "token");
  }

  auto asked_for(std::string_view part) const -> bool {
    return std::ranges::any_of(asked, [part](const std::string& url) {
      return url.find(part) != std::string::npos;
    });
  }

  std::vector<std::pair<std::string, std::string>> graphql_replies;
  std::vector<std::string> asked;
  unsigned no_game_refusals{};
  unsigned busy_locks{};
  unsigned unscripted{};

 private:
  [[nodiscard]] auto graphql(std::string_view url) const -> Reply {
    for (const auto& [operation, body] : graphql_replies)
      if (url.find(operation) != std::string_view::npos)
        return replied(200, body);
    return replied(404, {});
  }

  [[nodiscard]] static auto redirected(std::string_view location) -> Reply {
    return replied(302, {}, {"Location", location});
  }

  [[nodiscard]] static auto replied(
      std::uint16_t status, std::string_view body,
      std::pair<std::string_view, std::string_view> header = {}) -> Reply {
    libchiaking::platform::HttpResponse response;
    response.status_code = status;
    if (!header.first.empty())
      response.headers.push_back({std::string{header.first},
                                  libchiaking::SecretString{header.second}});
    response.body.assign(body);
    return response;
  }
};

class TranscriptRig {
 public:
  static constexpr std::uint64_t account = 1;

  explicit TranscriptRig(std::string_view golden) : golden_(golden) {
    ps_.conceal_in(transcript_);
  }

  TranscriptRig(const TranscriptRig&) = delete;
  auto operator=(const TranscriptRig&) -> TranscriptRig& = delete;

  auto ps() -> TranscriptPs& { return ps_; }
  auto waiter() const -> const FakeWaiter& { return waiter_; }

  auto configuration(std::string_view title)
      -> libchiaking::play::CloudLinkConfiguration {
    libchiaking::play::CloudLinkConfiguration config;
    config.catalog = &catalog_;
    config.product = test_support::play::Title::product_id(title);
    config.time_zone = "UTC+00:00";
    config.datacenter = libchiaking::play::DatacenterPreference::named(
        libchiaking::play::DatacenterName::parse("dc1").value());
    return config;
  }

  void hold(std::string_view region, HeldCatalog::Listed listed) {
    HeldCatalog::hold(records_, wall_, libchiaking::AccountId{account}, region,
                      {listed});
  }

  auto signed_in_user(std::string_view country = "GB",
                      bool states_its_audience = false)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    auto saved =
        SignedInPlayer::saved(TranscriptPs::npsso, TranscriptPs::device_id,
                              libchiaking::AccountId{account});
    saved.region = libchiaking::users::Region{std::string{country}, "en"};
    if (states_its_audience)
      saved.context = libchiaking::users::AccountContext::stating(
          {.legal_country = std::string{country},
           .locale = "en-" + std::string{country},
           .age = HeldCatalog::audience_age});
    auto user = users_.as(std::move(saved));
    libchiaking::psn::SessionInternals{
        libchiaking::users::PsUser::Core::of(*user).session()}
        .app_token()
        .supply(libchiaking::psn::auth::AppToken{TranscriptPs::app_token},
                std::chrono::system_clock::time_point{
                    std::chrono::milliseconds{0}});
    return user;
  }

  auto launch(libchiaking::play::CloudLinkConfiguration config,
              libchiaking::play::CloudLaunchSeams seams = {})
      -> ::testing::AssertionResult {
    seams.waiter = &waiter_;
    const auto allocation =
        CloudProvision::run(std::move(config), factory_, seams);
    if (!allocation)
      return ::testing::AssertionFailure()
             << golden_ << ": the scripted launch failed at stage "
             << static_cast<unsigned>(allocation.error().stage()) << ": "
             << allocation.error().message();
    if (allocation.value().server_ip != "203.0.113.5" ||
        allocation.value().server_port != 9295)
      return ::testing::AssertionFailure()
             << golden_ << ": the scripted launch reached an unexpected server";
    if (ps_.unscripted != 0)
      return ::testing::AssertionFailure() << golden_ << ": " << ps_.unscripted
                                           << " requests had no scripted reply";
    if (!play_transcript::GoldenFile{golden_}.holds(transcript_))
      return ::testing::AssertionFailure()
             << golden_ << ": the transcript differs from the golden file";
    return ::testing::AssertionSuccess();
  }

 private:
  TranscriptPs ps_;
  FakeWaiter waiter_;
  std::string golden_;
  play_transcript::Transcript transcript_;
  play_transcript::TranscribingHttp http_{ps_, transcript_};
  remote_start_script::ScriptClock clock_;
  remote_start_script::ScriptRandom random_;
  remote_start_script::RejectCrypto crypto_;
  public_route_fixture::RouteNetwork unreachable_;
  play_transcript::TranscribingNetwork network_{unreachable_, transcript_};
  libchiaking::platform::DefaultPlatformFactory factory_{
      libchiaking::platform::PlatformProviders{.clock = &clock_,
                                               .random = &random_,
                                               .network = &network_,
                                               .crypto = &crypto_,
                                               .http = &http_}};
  libchiaking::testing::ManualClock monotonic_;
  libchiaking::testing::ManualClock wall_;
  test_support::users::SignedIn users_{http_, monotonic_, wall_};
  libchiaking::testing::InMemoryRecordStore records_{
      LiveProviders::threading()};
  libchiaking::users::RequestBudget budget_{LiveProviders::threading(),
                                            monotonic_};
  libchiaking::catalog::Catalog catalog_{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::catalog::Catalog>(http_, records_,
                                         LiveProviders::threading(), monotonic_,
                                         wall_, budget_)};
};

}  // namespace play_test::cloud_transcripts
