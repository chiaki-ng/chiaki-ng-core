#pragma once

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "tests/support/websocket.hpp"
#include "tests/support/log.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace test_support::api {

class OfflineHttp final : public libchiaking::platform::HttpProvider {
 public:
  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest& request,
                             const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    for (const auto& header : request.headers)
      sent += std::string{header.value.view()};
    sent += std::string{request.url.view()};
    return libchiaking::unexpected(
        stop.stop_requested() ? libchiaking::platform::HttpError::cancelled
                              : libchiaking::platform::HttpError::disconnected);
  }

  int requests{};
  std::string sent;
};

class InterruptibleHttp final : public libchiaking::platform::HttpProvider {
 public:
  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest&,
                             const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    if (interrupt) interrupt();
    if (answer && !stop.stop_requested()) {
      libchiaking::platform::HttpResponse response;
      response.status_code = 200;
      response.body.assign(*answer);
      return response;
    }
    return libchiaking::unexpected(
        stop.stop_requested() ? libchiaking::platform::HttpError::cancelled
                              : libchiaking::platform::HttpError::disconnected);
  }

  std::optional<std::string> answer;
  int requests{};
  std::function<void()> interrupt;
};

}  // namespace test_support::api
