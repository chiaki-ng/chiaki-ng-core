#include <libchiaking/platform/http/curl/selected_provider.hpp>

#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/recording_diagnostics.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"
#include "tests/unit/platform/http/curl/silent_http_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace platform_curl_test {
namespace {

using namespace std::chrono_literals;

class CurlMultiQueue : public CurlHttpRig {
 protected:
  static void expect_started_requests_overlap(HttpProvider& provider) {
    SilentHttpServer server;
    ASSERT_TRUE(server.ready());
    std::vector<PendingHttpResponse> pending;
    const auto started = now();
    for (int index = 0; index < 4; ++index) {
      auto request = get(server.url("/overlap"));
      request.total_timeout = 500ms;
      pending.push_back(provider.start(request, {}));
    }
    for (auto& waiting : pending) {
      const auto result = std::move(waiting).wait();
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), HttpError::timed_out);
    }
    EXPECT_LE(now() - started, 1500ms);
  }
};

TEST_F(CurlMultiQueue, LogsTheQueueAndTheStallWithoutTheSecretUrl) {
  SilentHttpServer server;
  ASSERT_TRUE(server.ready());
  RecordingDiagnostics log;
  log.hold_once("share at most", 1500ms);
  CurlMultiSettings settings;
  settings.max_total_connections = 1;
  settings.stall_timeout = 300ms;
  auto provider =
      CurlMultiHttpProvider::create(settings, threading(), clock(), &log);
  ASSERT_TRUE(provider);
  std::vector<std::optional<HttpError>> errors(2);
  std::vector<Thread> callers;
  for (std::size_t index = 0; index < errors.size(); ++index) {
    auto caller = Thread::start(
        threading(), "stall caller", ThreadRole::unspecified, [&, index] {
          auto request = get(server.url("/secret-path?token=hidden"));
          request.label = "stall probe";
          const auto result = provider.value()->perform(request, {});
          if (!result) errors[index] = result.error();
        });
    ASSERT_TRUE(caller);
    callers.push_back(std::move(caller).value());
  }
  for (auto& caller : callers) static_cast<void>(caller.join());
  for (const auto& error : errors) EXPECT_EQ(error, HttpError::timed_out);
  EXPECT_EQ(log.count(LogSeverity::verbose, "share at most 1 connections"), 1U);
  EXPECT_EQ(log.count(LogSeverity::warning, "has not turned"), 1U);
  EXPECT_EQ(log.count(LogSeverity::error, "stall probe to 127.0.0.1 gave up"),
            1U);
  EXPECT_FALSE(log.mentions("secret-path"));
  EXPECT_FALSE(log.mentions("hidden"));
}

TEST_F(CurlMultiQueue, OverlapsStartedRequests) {
  auto created = CurlMultiHttpProvider::create(CurlMultiSettings{}, threading(),
                                               clock(), nullptr);
  ASSERT_TRUE(created);
  expect_started_requests_overlap(*created.value());
}

TEST_F(CurlMultiQueue, OverlapsStartedRequestsThroughTheSelectedProvider) {
  SelectedCurlHttpProvider selected(
      HttpTransportSettings{.transport = HttpTransport::multi}, threading(),
      clock());
  expect_started_requests_overlap(selected);
}

TEST_F(CurlMultiQueue, DropsARequestNobodyWaitsFor) {
  SilentHttpServer server;
  KeepAliveHttpServer answering(1);
  ASSERT_TRUE(server.ready());
  ASSERT_TRUE(answering.ready());
  auto created = CurlMultiHttpProvider::create(CurlMultiSettings{}, threading(),
                                               clock(), nullptr);
  ASSERT_TRUE(created);
  auto& provider = *created.value();
  {
    auto request = get(server.url("/abandoned"));
    request.total_timeout = 30s;
    const auto abandoned = provider.start(request, {});
  }
  const auto started = now();
  const auto result = provider.perform(get(answering.url("/after")), {});
  answering.join();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().status_code, 200);
  created.value().reset();
  EXPECT_LE(now() - started, 2s);
}

}  // namespace
}  // namespace platform_curl_test
