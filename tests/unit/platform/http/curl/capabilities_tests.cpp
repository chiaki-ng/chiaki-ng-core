#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/recording_diagnostics.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace platform_curl_test {
namespace {

class CurlMultiCapabilities : public CurlHttpRig {};

TEST_F(CurlMultiCapabilities, DetectsMultiAVersionAndAnyExpectedConstraint) {
  const auto capabilities = CurlCapabilities::detect();
  EXPECT_TRUE(capabilities.multi);
  EXPECT_FALSE(capabilities.version.empty());
  if (std::getenv("LIBCHIAKING_EXPECT_CONSTRAINED_CURL") != nullptr) {
    EXPECT_FALSE(capabilities.wakeup);
    EXPECT_FALSE(capabilities.asynchronous_dns);
    EXPECT_FALSE(capabilities.http2);
  }
}

TEST_F(CurlMultiCapabilities, LogsEachMissingCapabilityOnce) {
  RecordingDiagnostics log;
  auto provider = CurlMultiHttpProvider::create(
      CurlMultiSettings{}, without_extras(), threading(), clock(), &log);
  ASSERT_TRUE(provider);
  EXPECT_EQ(provider.value()->dns(), CurlDnsStrategy::caller);
  EXPECT_EQ(log.count(LogSeverity::warning, "cannot be woken"), 1U);
  EXPECT_EQ(log.count(LogSeverity::warning, "callers' threads"), 1U);
  EXPECT_EQ(log.count(LogSeverity::info, "no HTTP/2"), 1U);
  EXPECT_EQ(log.count(LogSeverity::warning, "handshakes in blocking mode"), 1U);
}

TEST_F(CurlMultiCapabilities, DoesNotWarnAboutCapabilitiesItHas) {
  RecordingDiagnostics log;
  auto provider = CurlMultiHttpProvider::create(CurlMultiSettings{},
                                                threading(), clock(), &log);
  ASSERT_TRUE(provider);
  const auto& capabilities = provider.value()->capabilities();
  if (capabilities.wakeup) EXPECT_FALSE(log.mentions("cannot be woken"));
  if (capabilities.asynchronous_dns)
    EXPECT_FALSE(log.mentions("callers' threads"));
}

TEST_F(CurlMultiCapabilities, ResolvesNamesOnACallersThreadWithoutAsyncDns) {
  KeepAliveHttpServer server(1);
  ASSERT_TRUE(server.ready());
  RecordingDiagnostics log;
  auto provider = CurlMultiHttpProvider::create(
      CurlMultiSettings{}, without_extras(), threading(), clock(), &log);
  ASSERT_TRUE(provider);
  const auto result = provider.value()->perform(
      get("http://localhost:" + std::to_string(server.port()) + "/named"), {});
  server.join();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().status_code, 200);
  EXPECT_EQ(log.count(LogSeverity::verbose,
                      "resolved localhost on a caller's thread"),
            1U);
}

}  // namespace
}  // namespace platform_curl_test
