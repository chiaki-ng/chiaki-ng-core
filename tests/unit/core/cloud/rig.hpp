#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/links/cloud/server_reservation.hpp"
#include "src/core/psn/gk/client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {

namespace gk = libchiaking::psn::gk;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
using libchiaking::cloud::CloudPlatform;
using libchiaking::cloud::CloudService;

class RecordingHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    last = request.clone();
    ++calls;
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    for (const auto& [name, value] : reply_headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    response.body = libchiaking::SecretBytes{std::string_view{reply_body}};
    return response;
  }

  auto body() const -> std::string_view { return last.body.view(); }

  platform::HttpRequest last;
  std::uint16_t status{200};
  std::string reply_body;
  std::vector<std::pair<std::string, std::string>> reply_headers;
  std::optional<platform::HttpError> error;
  unsigned calls{};
};

class SelectProvider final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_NE(request.url.view().find("/datacenters/select"),
              std::string::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = status;
    for (const auto& [name, value] : headers)
      response.headers.push_back({name, libchiaking::SecretString{value}});
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
  std::string body{R"({"port":8975})"};
  std::vector<std::pair<std::string, std::string>> headers{};
  unsigned calls{};
};

class HttpHolder {
 public:
  HttpHolder(platform::HttpProvider& provider, platform::StopToken stop)
      : http{provider, clock, std::move(stop)} {}

  platform::SteadyClock clock;
  libchiaking::net::http::Client http;
};

class Gk : private HttpHolder, public gk::Client {
 public:
  explicit Gk(platform::HttpProvider& provider, platform::StopToken stop = {})
      : HttpHolder{provider, std::move(stop)}, gk::Client{HttpHolder::http} {}

  Gk(const Gk&) = delete;
  auto operator=(const Gk&) -> Gk& = delete;
};

class GkRig {
 public:
  static constexpr std::string_view blank_spec =
      R"({"gkCloudAuthCode":"","ps3AuthCode":"","streamServerAuthCode":""})";

  static auto session_id(std::string_view value) -> gk::SessionId {
    return gk::SessionId{value};
  }

  static auto config_key(std::string_view value) -> gk::ConfigKey {
    return gk::ConfigKey{value};
  }

  static auto launch_spec(CloudService service) -> gk::LaunchSpec {
    const auto zone = gk::TimeZone::parse("UTC+00:00").value();
    if (service == CloudService::legacy_cloud_stream)
      return gk::LaunchSpec::legacy_cloud_stream(
                 "synthetic-entitlement", gk::Language{"en-US"}, zone, 720)
          .value();
    return gk::LaunchSpec::cloud_stream("synthetic-entitlement",
                                        gk::Language{"en-US"}, zone, 1080)
        .value();
  }

  static auto codes(std::string_view client = "CLIENT",
                    std::string_view server = "SERVER")
      -> libchiaking::psn::auth::GkCodes {
    return libchiaking::psn::auth::GkCodes{
        libchiaking::psn::auth::AuthCode{client},
        libchiaking::psn::auth::AuthCode{server}};
  }

  template <class Value>
  static auto failed(const libchiaking::Expected<Value, gk::Error>& result,
                     gk::Error::Stage stage, gk::Error::Reason reason,
                     std::uint32_t detail) -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the call succeeded";
    if (result.error().stage() != stage)
      return testing::AssertionFailure()
             << "failed in stage "
             << testing::PrintToString(result.error().stage());
    if (result.error().reason() != reason)
      return testing::AssertionFailure()
             << "failed for "
             << testing::PrintToString(result.error().reason());
    if (result.error().detail() != detail)
      return testing::AssertionFailure() << "detail " << result.error().detail()
                                         << " instead of " << detail;
    return testing::AssertionSuccess();
  }

  template <class Value>
  static auto fails_for(const libchiaking::Expected<Value, gk::Error>& result,
                        gk::Error::Reason reason) -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "the call succeeded";
    if (result.error().reason() != reason)
      return testing::AssertionFailure()
             << "failed for "
             << testing::PrintToString(result.error().reason());
    return testing::AssertionSuccess();
  }

  static auto recording_waiter() -> test_support::FakeWaiter {
    test_support::FakeWaiter waiter;
    waiter.answer = true;
    return waiter;
  }
};

class SessionSeed {
 public:
  enum class Until : std::uint8_t { started, authorized, locked };

  auto launch_spec() const -> gk::LaunchSpec {
    return spec ? gk::LaunchSpec::prebuilt(service, *spec)
                : GkRig::launch_spec(service);
  }

  std::string session_id{"synthetic-session"};
  std::string config_key{"synthetic-key"};
  std::optional<std::string> spec;
  gk::ClientIds ids{"gk-id", "ps3-id", "server-id"};
  CloudService service{CloudService::cloud_stream};
  CloudPlatform platform{CloudPlatform::ps5};
  Until until{Until::authorized};
};

class SessionHarness {
 public:
  SessionHarness(platform::HttpProvider& provider,
                 libchiaking::net::http::Waiter& waiter, SessionSeed seed = {},
                 std::shared_ptr<test_support::StopState> stop =
                     std::make_shared<test_support::StopState>(),
                 play::LaunchObserver* observer = nullptr,
                 const libchiaking::diagnostics::Trace* trace = nullptr)
      : stop_(std::move(stop)),
        trace_(trace),
        prelude_(provider, seed),
        gate_(observer),
        http_(prelude_, clock_, platform::StopToken{stop_}),
        client_(http_) {
    open(waiter, seed);
  }

  SessionHarness(const SessionHarness&) = delete;
  auto operator=(const SessionHarness&) -> SessionHarness& = delete;

  auto session() -> play::ServerReservation& { return session_.value(); }
  auto client() noexcept -> gk::Client& { return client_; }
  void stop() noexcept { stop_->stopped = true; }

 private:
  void open(libchiaking::net::http::Waiter& waiter, const SessionSeed& seed) {
    auto opened = play::ServerReservation::open(
        client_, {waiter, trace_, gate_}, seed.launch_spec(), seed.platform);
    ASSERT_TRUE(opened);
    session_.emplace(std::move(opened.value()));
    if (seed.until != SessionSeed::Until::started) {
      EXPECT_TRUE(session_->authorize(GkRig::codes()));
    }
    if (seed.until == SessionSeed::Until::locked) {
      EXPECT_TRUE(session_->lock());
    }
    prelude_.finish();
    gate_.open();
  }

  class Prelude final : public platform::HttpProvider {
   public:
    Prelude(platform::HttpProvider& inner, const SessionSeed& seed)
        : inner_(inner),
          session_id_(seed.session_id),
          config_key_(seed.config_key),
          ids_(seed.ids) {}

    void finish() noexcept { finished_ = true; }

    auto perform(const platform::HttpRequest& request,
                 const platform::StopToken& stop)
        -> libchiaking::Expected<platform::HttpResponse,
                                 platform::HttpError> override {
      if (finished_) return inner_.perform(request, stop);
      const auto url = request.url.view();
      platform::HttpResponse response;
      response.status_code = 200;
      auto reply = test_support::JsonTree::object();
      if (url.find("/client_ids") != std::string_view::npos) {
        reply["gkClientId"] = std::string{ids_.gk_client_id};
        reply["ps3GkClientId"] = std::string{ids_.ps3_gk_client_id};
        reply["streamServerClientId"] =
            std::string{ids_.stream_server_client_id};
      } else if (url.ends_with("/config")) {
        reply["configKey"] = std::string{config_key_};
      } else if (url.find("/sessions/start") != std::string_view::npos) {
        reply["sessionId"] = std::string{session_id_};
      } else if (url.find("/lock") != std::string_view::npos) {
        reply["lockAcquired"] = true;
      }
      const auto body = test_support::JsonTree::dump(reply);
      response.body = libchiaking::SecretBytes{std::string_view{body}};
      return response;
    }

   private:
    platform::HttpProvider& inner_;
    std::string session_id_;
    std::string config_key_;
    gk::ClientIds ids_;
    bool finished_{};
  };

  class Gate final : public play::LaunchObserver {
   public:
    explicit Gate(play::LaunchObserver* target) noexcept : target_(target) {}

    void open() noexcept { open_ = true; }

    void on_progress(
        const play::LaunchProgress& progress,
        libchiaking::net::protocol::remote_play::FailureStage stage,
        std::string_view text) override {
      if (open_ && target_) target_->on_progress(progress, stage, text);
    }

    void on_datacenters(const std::vector<gk::Ping>& picker_rows,
                        const std::vector<gk::Ping>& measured_rows) override {
      if (open_ && target_) target_->on_datacenters(picker_rows, measured_rows);
    }

    void on_datacenter_selected(const gk::Ping& selected,
                                bool forced) override {
      if (open_ && target_) target_->on_datacenter_selected(selected, forced);
    }

    auto selects_datacenter_at_launch() const noexcept -> bool override {
      return open_ && target_ && target_->selects_datacenter_at_launch();
    }

    void offer_datacenters(const std::vector<std::string>& names) override {
      if (open_ && target_) target_->offer_datacenters(names);
    }

    auto await_datacenter_choice(const platform::StopToken& stop)
        -> play::LaunchChoice override {
      if (open_ && target_) return target_->await_datacenter_choice(stop);
      return play::LaunchObserver::await_datacenter_choice(stop);
    }

    auto datacenter_choice_wait() const noexcept
        -> std::optional<std::chrono::milliseconds> override {
      if (open_ && target_) return target_->datacenter_choice_wait();
      return play::LaunchObserver::datacenter_choice_wait();
    }

   private:
    play::LaunchObserver* target_;
    bool open_{};
  };

  std::shared_ptr<test_support::StopState> stop_;
  const libchiaking::diagnostics::Trace* trace_;
  Prelude prelude_;
  platform::SteadyClock clock_;
  Gate gate_;
  libchiaking::net::http::Client http_;
  gk::Client client_;
  std::optional<play::ServerReservation> session_;
};

}  // namespace cloud_test
