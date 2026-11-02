#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/json/json.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/users/rig.hpp"
#include "tests/unit/core/users/scripted_peer.hpp"

#include <libchiaking/users.hpp>

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

namespace users_test {

class OldServicePlayer {
 public:
  explicit OldServicePlayer(std::uint64_t account = 7) {
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{"SYNTHETIC"}};
    saved.device = libchiaking::users::DeviceId::parse(duid);
    user = signed_in.as(std::move(saved));
  }

  static constexpr std::string_view duid =
      "0000000700410080000102030405060708090a0b0c0d0e0f";

  auto snapshot() const -> test_support::JsonTree::Tree {
    const auto text = user->snapshot_json().value();
    EXPECT_EQ(text.find("COMMERCE"), std::string::npos) << text;
    EXPECT_EQ(text.find("SYNTHETIC"), std::string::npos) << text;
    auto parsed = test_support::JsonTree::parse(text);
    EXPECT_TRUE(parsed) << text;
    return parsed ? std::move(parsed.value())
                  : test_support::JsonTree::object();
  }

  test_support::users::Offline offline;
  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::users::SignedIn signed_in{offline, clock, wall};
  std::shared_ptr<libchiaking::users::PsUser> user;
};

class OldServiceRig {
 public:
  using Step = libchiaking::users::OldServiceRefusal::Step;
  using Sent = ScriptedStep;
  using Request = libchiaking::platform::HttpRequest;
  using Method = libchiaking::platform::HttpMethod;

  static constexpr std::string_view duid = OldServicePlayer::duid;
  static constexpr std::string_view product = "EP0000-CUSA12345_00-TITLE";
  static constexpr std::string_view entitlement = "EP0000-CUSA12345_00-STREAM";
  static constexpr std::string_view container =
      R"({"default_sku":{"id":"SKU-A","entitlements":[{"id":"EP0000-CUSA12345_00-STREAM","license_type":4,"packageType":"PS4GS"}]},"playable_platform":["PS4"]})";
  static constexpr std::string_view success =
      R"({"header":{"status_code":"0x0000"}})";

  struct Asked {
    bool owned{};
    bool skip_account_attributes{};
    std::vector<Step>* announced{};
  };

  struct Launched {
    auto finished() const -> bool {
      return !refusal && !title && account_accepted;
    }

    auto cancelled() const -> bool {
      if (refusal) return refusal->is_cancelled();
      return title && title->failure() && title->failure()->is_cancelled();
    }

    std::optional<libchiaking::users::OldServiceRefusal> refusal;
    std::optional<libchiaking::psn::rest::StoreError> title;
    std::optional<std::uint16_t> licence;
    std::optional<libchiaking::psn::rest::CheckoutPreview> preview;
    bool account_accepted{};
  };

  static auto text(const Request& request) -> std::string_view {
    return request.body.view();
  }

  static auto header(const Request& request, std::string_view name)
      -> std::string_view {
    return ScriptedPeer::header(request, name);
  }

  static auto oauth(std::string_view key, std::string_view value,
                    std::string_view marker = "response_type=code") -> Sent {
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
          EXPECT_NE(request.url.view().find(duid), std::string_view::npos)
              << request.url.view();
        }};
  }

  static auto session(bool anonymous) -> Sent {
    std::vector<ScriptedHeader> headers;
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

  static auto full_steps(bool acquire = true) -> std::vector<Sent> {
    std::vector<Sent> steps{
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
                     std::string_view::npos)
               << text(request);
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

  static auto without_token(std::vector<Sent> steps) -> std::vector<Sent> {
    steps.erase(steps.begin() + 3);
    return steps;
  }

  static auto bearer(std::string_view value)
      -> std::function<void(const Request&)> {
    return [expected = "Bearer " + std::string{value}](const Request& request) {
      EXPECT_EQ(header(request, "Authorization"), expected);
    };
  }

  static auto token_count(const ScriptedPeer& peer) -> int {
    return peer.count("response_type=token");
  }

  static auto code_count(const ScriptedPeer& peer) -> int {
    return peer.count("response_type=code");
  }

  static auto requested_buynow(const ScriptedPeer& peer) -> bool {
    return std::ranges::any_of(peer.urls, [](const std::string& url) {
      return url.ends_with("/user/checkout/buynow");
    });
  }

  static auto preview_body(std::string_view data) -> std::string {
    return std::string{R"({"header":{"status_code":"0x0000"},"data":)"} +
           std::string{data} + "}";
  }

  static auto client(ScriptedPeer& peer) -> libchiaking::net::http::Client {
    return libchiaking::net::http::Client{
        peer, Runtime::steady(), libchiaking::platform::StopToken{peer.stop}};
  }

  static auto launch(ScriptedPeer& peer, libchiaking::users::PsUser& user)
      -> Launched {
    return launch(peer, user, Asked{});
  }

  static auto launch(ScriptedPeer& peer, libchiaking::users::PsUser& user,
                     const Asked& asked) -> Launched {
    Launched launched;
    auto http = client(peer);
    libchiaking::users::PlayCalls calls{user};
    const std::function<void(Step)> announce = [&asked](Step step) {
      if (asked.announced != nullptr) asked.announced->push_back(step);
    };
    std::optional<libchiaking::psn::rest::KamajiClient> store;
    if (!asked.owned) {
      auto opened = calls.old_service_title_store(http, announce);
      if (!opened) {
        launched.refusal = opened.error();
        return launched;
      }
      store.emplace(std::move(opened.value()));
      const auto listed = store->title_container<test_support::JsonTree::Tree>(
          "GB", "en", product);
      if (!listed) {
        launched.title = listed.error();
        return launched;
      }
      auto checkout = calls.old_service_checkout(http);
      EXPECT_TRUE(checkout);
      if (!checkout) return launched;
      if (!asked.skip_account_attributes) {
        const auto accepted = checkout.value().account_attributes();
        if (!accepted) {
          launched.refusal = accepted.error();
          return launched;
        }
      }
      const auto status = checkout.value().licence_status(entitlement);
      if (!status) {
        launched.refusal = status.error();
        return launched;
      }
      launched.licence = status.value();
      if (status.value() == 404) {
        auto previewed = checkout.value().preview(*store, "SKU-A");
        if (!previewed) {
          launched.refusal = previewed.error();
          return launched;
        }
        launched.preview = previewed.value();
        if (!previewed.value().free) return launched;
        const auto bought = checkout.value().buy(
            *store, previewed.value().sku.empty() ? std::string{"SKU-A"}
                                                  : previewed.value().sku);
        if (!bought) {
          launched.refusal = bought.error();
          return launched;
        }
      } else if (status.value() != 200) {
        return launched;
      }
    }
    if (!store)
      store.emplace(http.limited(libchiaking::json::max_document_bytes));
    const auto account = calls.old_service_account_session(
        http, *store, libchiaking::psn::StreamingPlatform::ps4, announce);
    if (!account) {
      launched.refusal = account.error();
      return launched;
    }
    launched.account_accepted = account.value().accepted;
    return launched;
  }
};

}  // namespace users_test
