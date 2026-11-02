#pragma once

#include "libchiaking/detail/diagnostics/api_capture.hpp"
#include "libchiaking/detail/diagnostics/http_call_log.hpp"
#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/text/base64.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace diagnostics_test {

class Calls {
 public:
  using Outcome = libchiaking::Expected<libchiaking::platform::HttpResponse,
                                        libchiaking::platform::HttpError>;
  using MarkedText = libchiaking::diagnostics::HttpMarkedText;

  static auto request_to(std::string_view url, std::string_view body = {})
      -> libchiaking::platform::HttpRequest {
    libchiaking::platform::HttpRequest request;
    request.method = body.empty() ? libchiaking::platform::HttpMethod::get
                                  : libchiaking::platform::HttpMethod::post;
    request.url = libchiaking::SecretString{url};
    request.body = libchiaking::SecretBytes{
        std::as_bytes(std::span{body.data(), body.size()})};
    request.label = "test.call";
    return request;
  }

  static auto answered(std::uint16_t status, std::string_view body = {})
      -> Outcome {
    libchiaking::platform::HttpResponse response;
    response.status_code = status;
    response.body.assign(body);
    return response;
  }

  static void record(libchiaking::diagnostics::HttpCallLog& log,
                     const libchiaking::platform::HttpRequest& request,
                     const Outcome& outcome) {
    log.record(libchiaking::diagnostics::HttpArea::other, request, outcome,
               libchiaking::diagnostics::HttpCall::Clock::time_point{
                   std::chrono::microseconds{wall_clock.now_microseconds()}},
               std::chrono::microseconds{1500});
  }

  static auto secrets_in(const MarkedText& marked) -> std::vector<std::string> {
    std::vector<std::string> found;
    for (const auto& span : marked.spans())
      found.emplace_back(marked.text().substr(span.offset, span.length));
    return found;
  }

  static auto masked(const MarkedText& marked) -> std::string {
    std::string text{marked.text()};
    for (const auto& span : marked.spans())
      text.replace(span.offset, span.length, span.length, '*');
    return text;
  }

  static auto header(
      const std::vector<libchiaking::diagnostics::HttpMarkedHeader>& headers,
      std::string_view name) -> const MarkedText& {
    const auto found = std::ranges::find(
        headers, name, &libchiaking::diagnostics::HttpMarkedHeader::name);
    if (found != headers.end()) return found->value;
    ADD_FAILURE() << "no header named " << name;
    return none;
  }

  static auto only(
      std::initializer_list<libchiaking::platform::HttpCredential> kinds)
      -> libchiaking::platform::HttpCredentials {
    libchiaking::platform::HttpCredentials set;
    for (const auto kind : kinds) set = set.with(kind);
    return set;
  }

  static inline const libchiaking::platform::SystemClock wall_clock{};
  static inline const libchiaking::platform::SteadyClock monotonic{};

 private:
  static inline const MarkedText none{};
};

class RefusingHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest&,
               const libchiaking::platform::StopToken&)
      -> Calls::Outcome override {
    libchiaking::platform::HttpResponse response;
    response.status_code = 403;
    response.reason = "SYNTHETIC_SECRET_REASON";
    response.final_url =
        libchiaking::SecretString{"https://SYNTHETIC_SECRET_URL"};
    response.headers.push_back(
        {"Authorization",
         libchiaking::SecretString{"SYNTHETIC_SECRET_HEADER"}});
    const std::array body{std::byte{1}, std::byte{2}};
    response.body = libchiaking::SecretBytes{std::span<const std::byte>{body}};
    return response;
  }
};

class Captures {
 public:
  static auto drain(libchiaking::diagnostics::ApiCapture& capture)
      -> std::string {
    const auto size = capture.read({});
    std::string value(size, '\0');
    EXPECT_EQ(capture.read(value), size);
    return value;
  }

  static auto take(libchiaking::diagnostics::ApiCapture& capture)
      -> std::string {
    const auto bytes = capture.read({});
    EXPECT_GT(bytes, 0U);
    char short_buffer{};
    EXPECT_EQ(capture.read({&short_buffer, 1}), bytes);
    std::string value(bytes, '\0');
    EXPECT_EQ(capture.read(value), bytes);
    return value;
  }

  static auto base64(std::string_view text) -> std::string {
    return libchiaking::text::base64::encode(
        std::as_bytes(std::span{text}),
        libchiaking::text::base64::Alphabet::standard);
  }
};

}  // namespace diagnostics_test
