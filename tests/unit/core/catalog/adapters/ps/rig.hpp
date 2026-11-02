#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {

class FragmentHttp final : public libchiaking::platform::HttpProvider {
 public:
  struct Header {
    std::string name;
    std::string value;
  };

  struct Reply {
    Reply(int code, std::string text = {}, std::vector<Header> extra = {},
          bool error = false)
        : status(code),
          body(std::move(text)),
          headers(std::move(extra)),
          transport_error(error) {}

    int status;
    std::string body;
    std::vector<Header> headers;
    bool transport_error;
  };

  struct Rule {
    std::string method;
    std::string fragment;
    std::deque<Reply> replies;
  };

  struct Seen {
    std::string method;
    std::string url;
    std::vector<Header> headers;
    std::string body;
    std::chrono::milliseconds total_timeout{};
    std::chrono::milliseconds connect_timeout{};
  };

  static auto location(std::string_view value) -> std::vector<Header> {
    return {{"Location", std::string{value}}};
  }

  void on(std::string method, std::string fragment,
          std::vector<Reply> replies) {
    const libchiaking::platform::Lock lock{mutex_};
    rules.push_back({std::move(method),
                     std::move(fragment),
                     {replies.begin(), replies.end()}});
  }

  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    const libchiaking::platform::Lock lock{mutex_};
    events.push_back("perform " + std::string{request.url.view()});
    const std::string method =
        libchiaking::platform::http_method_name(request.method);
    std::vector<Header> headers;
    for (const auto& header : request.headers)
      headers.push_back({header.name, std::string{header.value.view()}});
    seen.push_back({method, std::string{request.url.view()}, std::move(headers),
                    std::string{request.body.view()},
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        request.total_timeout),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        request.connect_timeout)});
    EXPECT_EQ(request.redirect_policy,
              libchiaking::platform::HttpRedirectPolicy::deny);
    EXPECT_NE(request.maximum_response_bytes, 0U);
    if (stop.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    for (auto& rule : rules) {
      if (rule.method != method ||
          request.url.view().find(rule.fragment) == std::string_view::npos)
        continue;
      Reply reply = rule.replies.front();
      if (rule.replies.size() > 1) rule.replies.pop_front();
      if (reply.transport_error)
        return libchiaking::unexpected(
            libchiaking::platform::HttpError::disconnected);
      libchiaking::platform::HttpResponse response;
      response.status_code = static_cast<std::uint16_t>(reply.status);
      for (const auto& header : reply.headers)
        response.headers.push_back(
            {header.name, libchiaking::SecretString{header.value}});
      response.body.assign(std::string_view{reply.body});
      return response;
    }
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  auto start(const libchiaking::platform::HttpRequest& request,
             const libchiaking::platform::StopToken& stop)
      -> libchiaking::platform::PendingHttpResponse override {
    {
      const libchiaking::platform::Lock lock{mutex_};
      events.push_back("start " + std::string{request.url.view()});
    }
    return libchiaking::platform::HttpProvider::start(request, stop);
  }

  auto events_naming(std::string_view fragment) const
      -> std::vector<std::string> {
    const libchiaking::platform::Lock lock{mutex_};
    std::vector<std::string> named;
    for (const auto& event : events) {
      const auto at = event.find(fragment);
      if (at == std::string::npos) continue;
      const auto end = event.find("&size", at);
      named.push_back(event.substr(0, event.find(' ')) + " " +
                      event.substr(at, end - at));
    }
    return named;
  }

  auto count(std::string_view fragment) const -> int {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<int>(
        std::ranges::count_if(seen, [fragment](const Seen& request) {
          return request.url.find(fragment) != std::string::npos;
        }));
  }

  std::vector<Rule> rules;
  std::vector<Seen> seen;
  std::vector<std::string> events;

 private:
  libchiaking::platform::DefaultThreadingProvider threading_;
  mutable libchiaking::platform::Mutex mutex_{threading_};
};

class PsRig {
 public:
  using Header = FragmentHttp::Header;
  using Reply = FragmentHttp::Reply;

  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;
  static constexpr std::string_view device =
      "000000070041008012345678901234567890123456789012";

  PsRig() {
    wall.now_us = static_cast<std::uint64_t>(wall_start_ms) * 1000U;
    waiter.stop_state = stop.get();
  }

  static auto location(std::string_view value) -> std::vector<Header> {
    return FragmentHttp::location(value);
  }

  static auto product(std::string_view id) -> libchiaking::catalog::ProductId {
    auto parsed = libchiaking::catalog::ProductId::parse(id);
    EXPECT_TRUE(parsed) << id;
    return std::move(parsed).value();
  }

  static auto locale(std::string_view text)
      -> libchiaking::catalog::StoreLocale {
    auto parsed = libchiaking::catalog::StoreLocale::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  auto user(std::uint64_t account = 1)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{"NPSSO"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app"},
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{wall_start_ms + 3'600'000}});
    saved.device = libchiaking::users::DeviceId::parse(device);
    EXPECT_TRUE(saved.device);
    return signed_in.as(std::move(saved));
  }

  auto home_user(std::uint64_t account = 9)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    auto added = signed_in.users.add_home_user(libchiaking::AccountId{account},
                                               "Living Room Player");
    EXPECT_TRUE(added);
    return std::move(added).value();
  }

  auto now() const -> std::chrono::sys_time<std::chrono::milliseconds> {
    return std::chrono::sys_time<std::chrono::milliseconds>{
        std::chrono::milliseconds{wall_start_ms}};
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  FragmentHttp http;
  test_support::FakeWaiter waiter;
  std::shared_ptr<test_support::StopState> stop =
      std::make_shared<test_support::StopState>();
  test_support::users::SignedIn signed_in{http, clock, wall};
  libchiaking::net::http::Client client{
      http, clock, libchiaking::platform::StopToken{stop}, nullptr, &waiter};
};

}  // namespace catalog_test
