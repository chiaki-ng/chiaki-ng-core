#pragma once

#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>
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
#include <tuple>
#include <type_traits>
#include <utility>

namespace users_test {

class Runtime {
 public:
  static auto threading() -> libchiaking::platform::DefaultThreadingProvider& {
    return test_support::process_threading();
  }

  static auto steady() -> const libchiaking::platform::SteadyClock& {
    static const libchiaking::platform::SteadyClock clock;
    return clock;
  }

  static void nap(std::chrono::milliseconds duration) {
    static libchiaking::platform::Sleeper sleeper{threading(), steady()};
    sleeper.sleep_for(duration);
  }

  template <class Question>
  static auto asked_together(test_support::psn::FakeHttp& fake,
                             std::string method, std::string url,
                             test_support::psn::Reply slow,
                             bool second_gives_up, const Question& question) {
    const auto watched =
        std::make_shared<test_support::WatchedStop>(second_gives_up);
    const auto started = std::make_shared<std::atomic<bool>>(false);
    slow.during = [started, watched] {
      *started = true;
      while (watched->asked == 0) nap(std::chrono::milliseconds{1});
    };
    fake.on(std::move(method), std::move(url), {std::move(slow)});
    using Answer =
        std::invoke_result_t<const Question&,
                             const libchiaking::platform::StopToken&>;
    std::optional<Answer> first;
    auto leader =
        libchiaking::platform::Thread::start(
            threading(), "test", libchiaking::platform::ThreadRole::unspecified,
            [&] {
              first.emplace(question(libchiaking::platform::StopToken{}));
            })
            .value();
    while (!*started) nap(std::chrono::milliseconds{1});
    auto second = question(libchiaking::platform::StopToken{watched});
    static_cast<void>(leader.join());
    return std::tuple{std::move(*first), std::move(second)};
  }
};

class Psn {
 public:
  using Reply = test_support::psn::Reply;

  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;
  static constexpr std::string_view subscriptions_url =
      "https://web.np.playstation.com/api/graphql/v1/"
      "op?operationName=getSubscriptions&variables=%7B%7D&extensions="
      "%7B%22persistedQuery%22%3A%7B%22version%22%3A1%2C%22sha256Hash%22%3A%"
      "22"
      "36c5aa5049115a8b37008e45815c3cd31414d52660cd94a45fcdb17294800db1%22%7D%"
      "7D";
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view legacy_token_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
  static constexpr std::string_view account_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token/"
      "remote-access";
  static constexpr std::string_view duid =
      "000000070041008012345678901234567890123456789012";
  static constexpr std::string_view correlation =
      "12345678-1234-1234-1234-123456789012";
  static constexpr std::string_view remote_authorize_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorize"
      "?client_id=ba495a24-818c-472b-b12d-ff231c1b5745"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp%20referenceDataService%3AcountryConfig.read"
      "%20pushNotification%3AwebSocket.desktop.connect%20sessionManager%"
      "3AremotePlaySession.system.update"
      "&response_type=code&access_type=offline&service_entity=urn%3Aservice-"
      "entity%3Apsn"
      "&duid=000000070041008012345678901234567890123456789012"
      "&smcid=remoteplay&layout_type=popup&PlatformPrivacyWs1=minimal&no_"
      "captcha=true"
      "&cid=12345678-1234-1234-1234-123456789012";
  static constexpr std::string_view app_refresh_body =
      "grant_type=refresh_token&refresh_token=old-mobile"
      "&scope=psn%3Amobile.v2.core+psn%3Aclientapp&token_format=jwt";
  static constexpr std::string_view remote_refresh_body =
      "grant_type=refresh_token&refresh_token=old-remote"
      "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2Fremoteplay%"
      "2Fredirect"
      "&scope=psn%3Aclientapp+referenceDataService%3AcountryConfig.read"
      "+pushNotification%3AwebSocket.desktop.connect+sessionManager%"
      "3AremotePlaySession.system.update";

  static auto at_ms(std::int64_t milliseconds)
      -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{milliseconds}};
  }

  static auto redirect(std::string_view location) -> Reply {
    Reply reply{302, ""};
    reply.headers.push_back({"Location", std::string{location}});
    return reply;
  }

  static auto subscription_entry(std::string_view label, std::string_view sku)
      -> std::string {
    return R"({"data":{"fetchSubscriptions":{"subscriptions":[{"active":true,"endDate":"2027-01-01T00:00:00.000Z",)"
           R"("id":")" +
           std::string{sku} +
           R"(","productName":"PlayStation Plus","subscriptionType":"PS_PLUS",)"
           R"("tierLabel":")" +
           std::string{label} + R"("}]}}})";
  }

  static auto exchanges_npsso(const test_support::psn::Seen& seen) -> bool {
    return seen.url == token_url &&
           seen.body.find("grant_type=sso_token") != std::string::npos &&
           seen.body.find("npsso=") != std::string::npos;
  }

  static auto own_device() -> libchiaking::users::DeviceId {
    return test_support::Setup::value(libchiaking::users::DeviceId::parse(duid),
                                      "the device id parses");
  }
};

class SignedInRig {
 public:
  SignedInRig() {
    wall.now_us = static_cast<std::uint64_t>(Psn::wall_start_ms) * 1000U;
  }

  auto saved(std::uint64_t account, std::string_view npsso,
             std::string_view store_token) const
      -> libchiaking::users::SavedSignIn {
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{account},
                                          "player",
                                          libchiaking::psn::auth::Npsso{npsso}};
    saved.store.emplace(libchiaking::psn::auth::AppToken{store_token},
                        Psn::at_ms(Psn::wall_start_ms + 3'600'000));
    return saved;
  }

  auto tiers() const -> int {
    return fake.count("operationName=getSubscriptions&");
  }

  void answers_a_sign_in_as_tokyo_player() {
    fake.on("GET", std::string{Psn::remote_authorize_url},
            {Psn::redirect("https://remoteplay.dl.playstation.net/remoteplay/"
                           "redirect?code=remote%2Bcode")});
    fake.on(
        "POST", std::string{Psn::token_url},
        {Psn::Reply{
             200,
             R"({"access_token":"remote-access","refresh_token":"remote-refresh","expires_in":3600,"token_type":"bearer"})"},
         Psn::Reply{
             200,
             R"({"access_token":"mobile-access","refresh_token":"mobile-refresh","expires_in":7200})"}});
    fake.on(
        "GET", std::string{Psn::account_url},
        {Psn::Reply{
            200,
            R"({"user_id":"578437695752307201","online_id":"Tokyo Player"})"}});
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp fake{&clock};
  test_support::users::SignedIn signed_in{fake, clock, wall};
};

}  // namespace users_test
