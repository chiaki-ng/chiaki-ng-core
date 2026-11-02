#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/log.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"

#include <gtest/gtest.h>

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::psn {

struct TestHeader {
  std::string name;
  std::string value;
};

struct Reply {
  Reply() = default;
  Reply(int code, std::string text) : status(code), body(std::move(text)) {}
  static auto transport() -> Reply {
    Reply reply;
    reply.transport_error = true;
    return reply;
  }

  int status{200};
  std::string body;
  std::vector<TestHeader> headers;
  bool transport_error{};
  std::int64_t elapsed_ms{};
  std::function<void()> during;
};

struct Seen {
  auto header(std::string_view name) const -> std::string {
    const auto found = std::ranges::find(headers, name, &TestHeader::name);
    return found == headers.end() ? std::string{} : found->value;
  }

  std::string method;
  std::string url;
  std::vector<TestHeader> headers;
  std::string body;
  std::chrono::milliseconds total_timeout{};
  std::chrono::milliseconds connect_timeout{};
  std::size_t maximum_response_bytes{};
  std::vector<std::uint16_t> expected_statuses;
};

class FakeHttp final : public libchiaking::platform::HttpProvider {
 public:
  explicit FakeHttp(test_support::FakeClock* clock = nullptr) : clock_(clock) {}

  void on(std::string method, std::string url, std::vector<Reply> replies) {
    rules.push_back(
        {std::move(method), std::move(url), {replies.begin(), replies.end()}});
  }

  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    const std::string method =
        libchiaking::platform::http_method_name(request.method);
    std::vector<TestHeader> headers;
    for (const auto& header : request.headers)
      headers.push_back({header.name, std::string{header.value.view()}});
    seen.push_back({method,
                    std::string{request.url.view()},
                    std::move(headers),
                    std::string{request.body.view()},
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        request.total_timeout),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        request.connect_timeout),
                    request.maximum_response_bytes,
                    {request.provenance.expected_statuses.begin(),
                     request.provenance.expected_statuses.end()}});
    EXPECT_EQ(request.redirect_policy,
              libchiaking::platform::HttpRedirectPolicy::deny);
    EXPECT_NE(request.maximum_response_bytes, 0U);
    if (stop.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    for (auto& rule : rules) {
      if (rule.method != method || rule.url != request.url.view()) continue;
      if (rule.replies.empty()) {
        ADD_FAILURE() << "no reply is left for " << request.url.view();
        return libchiaking::unexpected(
            libchiaking::platform::HttpError::disconnected);
      }
      Reply reply = rule.replies.front();
      if (rule.replies.size() > 1) rule.replies.pop_front();
      if (clock_ != nullptr) clock_->advance_ms(reply.elapsed_ms);
      if (reply.during) reply.during();
      if (reply.transport_error)
        return libchiaking::unexpected(
            libchiaking::platform::HttpError::disconnected);
      libchiaking::platform::HttpResponse response;
      response.status_code = static_cast<std::uint16_t>(reply.status);
      for (const auto& header : reply.headers)
        response.headers.push_back(
            {header.name, libchiaking::SecretString{header.value}});
      response.body.assign(reply.body);
      return response;
    }
    unmatched.push_back(std::string{request.url.view()});
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  auto count(std::string_view fragment) const -> int {
    return static_cast<int>(
        std::ranges::count_if(seen, [fragment](const Seen& request) {
          return request.url.find(fragment) != std::string::npos;
        }));
  }

  auto requests_to(std::string_view url) const -> int {
    return static_cast<int>(std::ranges::count(seen, url, &Seen::url));
  }

  struct Rule {
    std::string method;
    std::string url;
    std::deque<Reply> replies;
  };

  std::vector<Rule> rules;
  std::vector<Seen> seen;
  std::vector<std::string> unmatched;

 private:
  test_support::FakeClock* clock_;
};

}  // namespace test_support::psn
