#pragma once

#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/account/client.hpp"
#include "src/core/psn/session.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace accounts_test {

namespace account = libchiaking::psn::account;
namespace platform = libchiaking::platform;

class PsnSessionTest : public ::testing::Test {
 protected:
  auto open(std::string_view npsso)
      -> libchiaking::Expected<std::shared_ptr<libchiaking::psn::Session>,
                               libchiaking::psn::Session::Error> {
    return libchiaking::psn::Session::open(npsso, threading, clock, wall);
  }

  platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  platform::SteadyClock clock;
  platform::SystemClock wall;
};

class ScriptedHttp final : public platform::HttpProvider {
 public:
  ScriptedHttp(std::uint16_t status, std::string_view body)
      : status_(status), body_(body) {}
  explicit ScriptedHttp(platform::HttpError error) : error_(error) {}

  [[nodiscard]] auto perform(const platform::HttpRequest& request,
                             const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    urls.emplace_back(request.url.view());
    for (const auto& header : request.headers)
      headers.push_back(header.name + ": " + std::string{header.value.view()});
    if (error_) return libchiaking::unexpected(*error_);
    platform::HttpResponse response;
    response.status_code = status_;
    response.body.assign(std::as_bytes(std::span{body_.data(), body_.size()}));
    return response;
  }

  auto sent_header(std::string_view wanted) const -> bool {
    return std::ranges::find(headers, wanted) != headers.end();
  }

  std::vector<std::string> urls;
  std::vector<std::string> headers;

 private:
  std::uint16_t status_{};
  std::string body_;
  std::optional<platform::HttpError> error_;
};

class AccountClientTest : public ::testing::Test {
 protected:
  static constexpr std::string_view app_token = "fake-app-token-91c2";

  auto profile_failure(ScriptedHttp&& http, platform::StopToken stop = {})
      -> std::optional<account::Failure> {
    const auto profile = account::Client{http, clock, std::move(stop)}.profile(
        libchiaking::psn::auth::AppToken{app_token}, libchiaking::AccountId{7});
    if (profile) return std::nullopt;
    return profile.error();
  }

  platform::SteadyClock clock;
};

}  // namespace accounts_test
