#pragma once

#include "tests/support/stop_states.hpp"

#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

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

struct ScriptedHeader {
  std::string name;
  std::string value;
};

struct ScriptedStep {
  libchiaking::platform::HttpMethod method{
      libchiaking::platform::HttpMethod::get};
  std::string path;
  std::uint16_t status{200};
  std::string body;
  std::vector<ScriptedHeader> headers;
  std::function<void(const libchiaking::platform::HttpRequest&)> check;
};

class ScriptedPeer final : public libchiaking::platform::HttpProvider {
 public:
  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest& request,
                             const libchiaking::platform::StopToken& token)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    EXPECT_FALSE(token.stop_requested()) << request.url.view();
    urls.emplace_back(request.url.view());
    if (calls >= steps.size()) {
      ADD_FAILURE() << "unscripted request " << calls + 1 << " to "
                    << request.url.view();
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::backend_failure);
    }
    const auto& step = steps[calls++];
    EXPECT_EQ(request.method, step.method) << request.url.view();
    EXPECT_NE(request.url.view().find(step.path), std::string_view::npos)
        << request.url.view() << " does not reach " << step.path;
    EXPECT_EQ(request.redirect_policy,
              libchiaking::platform::HttpRedirectPolicy::deny)
        << request.url.view();
    if (step.check) step.check(request);
    if (fail_at && calls - 1 == *fail_at)
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::backend_failure);
    if (cancel_after && calls - 1 == *cancel_after) stop->stopped = true;
    libchiaking::platform::HttpResponse reply;
    reply.status_code = step.status;
    for (const auto& header : step.headers)
      reply.headers.push_back(
          {header.name, libchiaking::SecretString{header.value}});
    reply.body.assign(std::string_view{step.body});
    return reply;
  }

  auto count(std::string_view fragment) const -> int {
    return static_cast<int>(
        std::ranges::count_if(urls, [fragment](const std::string& url) {
          return url.find(fragment) != std::string::npos;
        }));
  }

  static auto header(const libchiaking::platform::HttpRequest& request,
                     std::string_view name) -> std::string_view {
    const auto found = std::ranges::find(
        request.headers, name,
        [](const auto& held) -> std::string_view { return held.name; });
    return found == request.headers.end() ? std::string_view{}
                                          : found->value.view();
  }

  std::vector<ScriptedStep> steps;
  std::size_t calls{};
  std::shared_ptr<test_support::StopState> stop{
      std::make_shared<test_support::StopState>()};
  std::optional<std::size_t> cancel_after;
  std::optional<std::size_t> fail_at;
  std::vector<std::string> urls;
};

}  // namespace users_test
