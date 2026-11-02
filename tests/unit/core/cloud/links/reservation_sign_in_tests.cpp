#include "libchiaking/detail/users/device_id.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;
using Stage = gk::Error::Stage;
using Texts = std::vector<std::string>;

class Progress final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&,
                   libchiaking::net::protocol::remote_play::FailureStage,
                   std::string_view text) override {
    texts.emplace_back(text);
  }

  std::vector<std::string> texts;
};

class SignIn {
 public:
  static constexpr std::string_view duid =
      "000000070041008000112233445566778899aabbccddeeff";

  SignIn(platform::HttpProvider& provider, SessionSeed seed,
         bool stopped = false, bool signed_out = false)
      : harness_(provider, waiter_, std::move(seed), stop_, &progress_),
        player_(test_support::play::SignedInPlayer::saved("synthetic-token",
                                                          duid)) {
    stop_->stopped = stopped;
    if (signed_out) {
      EXPECT_TRUE(player_.signed_in.users.sign_out(player_.user->account_id()));
    }
    libchiaking::net::http::Client account{provider, clock_,
                                           platform::StopToken{stop_}};
    libchiaking::users::PlayCalls user{*player_.user};
    result_ = harness_.session().sign_in(user, account);
  }

  static auto started(const gk::ClientIds& ids) -> SessionSeed {
    SessionSeed seed;
    seed.ids = ids;
    seed.until = SessionSeed::Until::started;
    return seed;
  }

  auto result() const noexcept
      -> const libchiaking::Expected<void, gk::Error>& {
    return result_;
  }

  auto progress() const noexcept -> const std::vector<std::string>& {
    return progress_.texts;
  }

  auto authorized_spec() -> std::optional<test_support::JsonTree::Tree> {
    auto parsed = test_support::JsonTree::parse(
        harness_.session().authorized_spec().view());
    if (!parsed || !parsed.value().is_object()) return std::nullopt;
    return std::move(parsed.value());
  }

 private:
  std::shared_ptr<test_support::StopState> stop_{
      std::make_shared<test_support::StopState>()};
  platform::SteadyClock clock_;
  test_support::FakeWaiter waiter_ = GkRig::recording_waiter();
  Progress progress_;
  SessionHarness harness_;
  test_support::play::SignedInPlayer player_;
  libchiaking::Expected<void, gk::Error> result_;
};

class OauthCodesProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    urls.emplace_back(request.url.view());
    if (request.method == platform::HttpMethod::post) {
      EXPECT_EQ(calls, 3U);
      EXPECT_TRUE(
          request.url.view().ends_with("/sessions/synthetic-session/authorize"))
          << request.url.view();
      platform::HttpResponse accepted;
      accepted.status_code = 200;
      return accepted;
    }
    EXPECT_EQ(request.method, platform::HttpMethod::get);
    EXPECT_EQ(request.redirect_policy, platform::HttpRedirectPolicy::deny);
    EXPECT_EQ(request.headers.size(), std::size_t{2});
    if (request.headers.size() > 1) {
      EXPECT_EQ(request.headers[0].name, "User-Agent");
      EXPECT_EQ(request.headers[1].value.view(), "npsso=synthetic-token");
    }
    if (fail_call == calls)
      return libchiaking::unexpected(platform::HttpError::backend_failure);
    if (cancel_call == calls)
      return libchiaking::unexpected(platform::HttpError::cancelled);
    platform::HttpResponse response;
    response.status_code = reject_call == calls ? 200 : 302;
    response.headers.push_back(
        {"Location",
         libchiaking::SecretString{
             (request.url.view().find("grc-response") != std::string_view::npos
                  ? std::string{"https://psnow.playstation.com/app/2.2.0/133/"
                                "5cdcc037d/grc-response.html"}
                  : std::string{"gaikai://local"}) +
             "?code=synthetic-" + std::to_string(calls)}});
    return response;
  }

  std::vector<std::string> urls;
  unsigned calls{};
  unsigned fail_call{};
  unsigned cancel_call{};
  unsigned reject_call{};
};

class ReservationOauthCodes : public ::testing::Test {
 protected:
  static auto ids() -> gk::ClientIds {
    return {"gk-id", "ps3-id", "server-id"};
  }

  static auto oauth_progress() -> Texts {
    return {"Getting Tokens - Step 4 of 10",
            "Getting Server Tokens - Step 5 of 10"};
  }

  OauthCodesProvider provider;
};

TEST_F(ReservationOauthCodes, SignsTheSpecWithTheClientAndServerCodes) {
  SignIn signed_in{provider, SignIn::started(ids())};
  EXPECT_TRUE(signed_in.result());
  EXPECT_EQ(provider.calls, 3U);
  const auto spec = signed_in.authorized_spec();
  ASSERT_TRUE(spec);
  EXPECT_EQ(test_support::JsonTree::text_at(*spec, "gkCloudAuthCode"),
            "synthetic-1");
  EXPECT_EQ(test_support::JsonTree::text_at(*spec, "streamServerAuthCode"),
            "synthetic-2");
  EXPECT_EQ(test_support::JsonTree::text_at(*spec, "ps3AuthCode"), "");
  ASSERT_GE(provider.urls.size(), std::size_t{2});
  EXPECT_NE(provider.urls[0].find("client_id=gk-id"), std::string::npos);
  EXPECT_NE(provider.urls[1].find("client_id=server-id"), std::string::npos);
  EXPECT_EQ(signed_in.progress(),
            (Texts{"Getting Tokens - Step 4 of 10",
                   "Getting Server Tokens - Step 5 of 10",
                   "Authorizing Session - Step 6 of 10"}));
}

TEST_F(ReservationOauthCodes, StopsAtATransportFailure) {
  provider.fail_call = 1;
  SignIn failed{provider, SignIn::started(ids())};
  EXPECT_TRUE(
      GkRig::failed(failed.result(), Stage::oauth, Reason::transport, 3));
  EXPECT_EQ(provider.calls, 1U);
  EXPECT_EQ(failed.progress(), Texts{"Getting Tokens - Step 4 of 10"});
}

TEST_F(ReservationOauthCodes, IsRejectedWithoutARedirect) {
  provider.reject_call = 2;
  SignIn refused{provider, SignIn::started(ids())};
  EXPECT_TRUE(
      GkRig::failed(refused.result(), Stage::oauth, Reason::rejected, 3));
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(refused.progress(), oauth_progress());
}

TEST_F(ReservationOauthCodes, IsCancelledWithTheRequest) {
  provider.cancel_call = 2;
  SignIn interrupted{provider, SignIn::started(ids())};
  const auto& codes = interrupted.result();
  ASSERT_FALSE(codes);
  EXPECT_EQ(codes.error().stage(), Stage::oauth);
  EXPECT_TRUE(codes.error().is_cancelled());
  EXPECT_EQ(codes.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(interrupted.progress(), oauth_progress());
}

TEST_F(ReservationOauthCodes, RefusesAClientIdThatCannotBeSent) {
  auto invalid = ids();
  invalid.stream_server_client_id = "not/an/id";
  SignIn unbuildable{provider, SignIn::started(invalid)};
  EXPECT_TRUE(GkRig::failed(unbuildable.result(), Stage::oauth,
                            Reason::invalid_authorization, 1));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationOauthCodes, RefusesAShortDuid) {
  EXPECT_FALSE(libchiaking::users::DeviceId::parse("short"));
}

TEST_F(ReservationOauthCodes, RefusesAnNpssoThatBreaksACookie) {
  EXPECT_FALSE(test_support::play::SignedInPlayer::admissible(
      test_support::play::SignedInPlayer::saved("token;other=1",
                                                SignIn::duid)));
}

TEST_F(ReservationOauthCodes, RefusesASignedOutUserBeforeSending) {
  SignIn unsendable{provider, SignIn::started(ids()), false, true};
  EXPECT_TRUE(GkRig::failed(unsendable.result(), Stage::oauth,
                            Reason::invalid_npsso, 2));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationOauthCodes, SendsNothingOnceStopped) {
  SignIn stopped{provider, SignIn::started(ids()), true};
  const auto& codes = stopped.result();
  ASSERT_FALSE(codes);
  EXPECT_EQ(codes.error().stage(), Stage::oauth);
  EXPECT_TRUE(codes.error().is_cancelled());
  EXPECT_EQ(codes.error().detail(), 0U);
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_TRUE(stopped.progress().empty());
}

class PrepareProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    if (request.method == platform::HttpMethod::post) {
      EXPECT_EQ(calls, 3U);
      EXPECT_TRUE(
          request.url.view().ends_with("/sessions/synthetic-session/authorize"))
          << request.url.view();
      platform::HttpResponse accepted;
      accepted.status_code = 200;
      return accepted;
    }
    EXPECT_EQ(request.method, platform::HttpMethod::get);
    EXPECT_GT(request.headers.size(), std::size_t{1});
    if (request.headers.size() > 1) {
      EXPECT_EQ(request.headers[1].value.view(), "npsso=synthetic-token");
    }
    EXPECT_NE(request.url.view().find(calls == 1 ? "client_id=gk-id"
                                                 : "client_id=server-id"),
              std::string::npos);
    platform::HttpResponse response;
    response.status_code = fail_second && calls == 2 ? 500 : 302;
    response.headers.push_back(
        {"Location",
         libchiaking::SecretString{
             (request.url.view().find("grc-response") != std::string_view::npos
                  ? std::string{"https://psnow.playstation.com/app/2.2.0/133/"
                                "5cdcc037d/grc-response.html"}
                  : std::string{"gaikai://local"}) +
             "?code=" + std::string(calls == 1 ? "CLIENT" : "SERVER")}});
    return response;
  }

  unsigned calls{};
  bool fail_second{};
};

class ReservationAuthPrepare : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto cloud_stream = gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", language, time_zone, 1080);
    ASSERT_TRUE(cloud_stream);
    cloud_stream_json = std::string{cloud_stream.value().json()};
    const auto legacy = gk::LaunchSpec::legacy_cloud_stream(
        "synthetic-entitlement", language, time_zone, 1080);
    ASSERT_TRUE(legacy);
    legacy_json = std::string{legacy.value().json()};
  }

  static auto seed(CloudService service, CloudPlatform target,
                   std::string_view spec) -> SessionSeed {
    auto result =
        SignIn::started(gk::ClientIds{"gk-id", "server-id", "server-id"});
    result.service = service;
    result.platform = target;
    result.spec = std::string{spec};
    return result;
  }

  auto legacy_seed() const -> SessionSeed {
    return seed(CloudService::legacy_cloud_stream, CloudPlatform::ps4,
                legacy_json);
  }

  const gk::Language language{"en-US"};
  const gk::TimeZone time_zone = gk::TimeZone::parse("UTC+00:00").value();
  std::string cloud_stream_json;
  std::string legacy_json;
  PrepareProvider provider;
};

TEST_F(ReservationAuthPrepare, FillsTheCloudStreamCodes) {
  SignIn authorized{provider, seed(CloudService::cloud_stream,
                                   CloudPlatform::ps5, cloud_stream_json)};
  EXPECT_TRUE(authorized.result());
  EXPECT_EQ(provider.calls, 3U);
  const auto parsed = authorized.authorized_spec();
  ASSERT_TRUE(parsed);
  EXPECT_EQ(test_support::JsonTree::text_at(*parsed, "gkCloudAuthCode"),
            "CLIENT");
  EXPECT_EQ(test_support::JsonTree::text_at(*parsed, "ps3AuthCode"), "");
  EXPECT_EQ(test_support::JsonTree::text_at(*parsed, "streamServerAuthCode"),
            "SERVER");
  EXPECT_EQ(authorized.progress(),
            (Texts{"Getting Tokens - Step 4 of 10",
                   "Getting Server Tokens - Step 5 of 10",
                   "Authorizing Session - Step 6 of 10"}));
}

TEST_F(ReservationAuthPrepare, FillsThePs3CodeForTheLegacyService) {
  SignIn authorized{provider, legacy_seed()};
  EXPECT_TRUE(authorized.result());
  EXPECT_EQ(provider.calls, 3U);
  const auto parsed = authorized.authorized_spec();
  ASSERT_TRUE(parsed);
  EXPECT_EQ(test_support::JsonTree::text_at(*parsed, "ps3AuthCode"), "SERVER");
}

TEST_F(ReservationAuthPrepare, StopsWhenTheServerCodeIsRefused) {
  provider.fail_second = true;
  SignIn authorized{provider, legacy_seed()};
  ASSERT_FALSE(authorized.result());
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(authorized.result().error().stage(), Stage::oauth);
  EXPECT_EQ(authorized.result().error().detail(), 3U);
}

TEST_F(ReservationAuthPrepare, SendsNothingOnceStopped) {
  provider.fail_second = true;
  SignIn authorized{provider, legacy_seed(), true};
  ASSERT_FALSE(authorized.result());
  EXPECT_EQ(provider.calls, 0U);
  EXPECT_EQ(authorized.result().error().stage(), Stage::oauth);
  EXPECT_TRUE(authorized.result().error().is_cancelled());
  EXPECT_EQ(authorized.result().error().detail(), 0U);
}

TEST_F(ReservationAuthPrepare, RefusesAShortDuidWithoutSending) {
  EXPECT_FALSE(libchiaking::users::DeviceId::parse("short"));
  EXPECT_EQ(provider.calls, 0U);
}

TEST_F(ReservationAuthPrepare, RefusesASpecWithoutCodeFieldsAfterTheCodes) {
  SignIn authorized{provider, seed(CloudService::legacy_cloud_stream,
                                   CloudPlatform::ps4, "{}")};
  ASSERT_FALSE(authorized.result());
  EXPECT_EQ(provider.calls, 2U);
  EXPECT_EQ(authorized.result().error().stage(), Stage::oauth);
  EXPECT_EQ(authorized.result().error().reason(), Reason::invalid_spec);
  EXPECT_EQ(authorized.result().error().detail(), 4U);
}

}  // namespace
}  // namespace cloud_test
