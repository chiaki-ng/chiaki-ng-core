#include <libchiaking/platform/http/curl/selected_provider.hpp>

#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/recording_diagnostics.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace platform_curl_test {
namespace {

class CurlHttpSelection : public CurlHttpRig {
 protected:
  static CurlCapabilities without_multi() {
    auto capabilities = CurlCapabilities::detect();
    capabilities.multi = false;
    return capabilities;
  }

  RecordingDiagnostics log;
};

TEST_F(CurlHttpSelection, KeepsThePooledTransportByDefaultWithoutLogging) {
  const SelectedCurlHttpProvider pooled(HttpTransportSettings{}, threading(),
                                        clock(), &log);
  EXPECT_EQ(pooled.transport(), HttpTransport::pooled);
  EXPECT_FALSE(log.mentions("http transport"));
}

TEST_F(CurlHttpSelection, LogsThatTheRequestedMultiTransportRuns) {
  const SelectedCurlHttpProvider multi(
      HttpTransportSettings{.transport = HttpTransport::multi}, threading(),
      clock(), &log);
  EXPECT_EQ(multi.transport(), HttpTransport::multi);
  EXPECT_EQ(log.count(LogSeverity::info, "multi runs"), 1U);
}

TEST_F(CurlHttpSelection, ChoosesPooledAutomaticallyWhenMultiIsMissing) {
  const SelectedCurlHttpProvider automatic(
      HttpTransportSettings{.transport = HttpTransport::automatic},
      without_multi(), threading(), clock(), &log);
  EXPECT_EQ(automatic.transport(), HttpTransport::pooled);
  EXPECT_EQ(log.count(LogSeverity::warning, "automatic chose pooled"), 1U);
}

TEST_F(CurlHttpSelection,
       FallsBackToPooledWithAnErrorWhenForcedMultiIsMissing) {
  const SelectedCurlHttpProvider forced(
      HttpTransportSettings{.transport = HttpTransport::multi}, without_multi(),
      threading(), clock(), &log);
  EXPECT_EQ(forced.transport(), HttpTransport::pooled);
  EXPECT_EQ(
      log.count(LogSeverity::error, "multi was requested but could not start"),
      1U);
}

TEST_F(CurlHttpSelection, ChoosesPooledAutomaticallyWhenHandshakesBlock) {
  const SelectedCurlHttpProvider cautious(
      HttpTransportSettings{
          .transport = HttpTransport::automatic,
          .tls_handshake_blocks = true,
      },
      threading(), clock(), &log);
  EXPECT_EQ(cautious.transport(), HttpTransport::pooled);
  EXPECT_FALSE(cautious.capabilities().asynchronous_tls_handshake);
  EXPECT_EQ(log.count(LogSeverity::warning,
                      "automatic chose pooled because the TLS backend "
                      "handshakes in blocking "
                      "mode"),
            1U);
}

TEST_F(CurlHttpSelection, KeepsAnInsistedMultiAndWarnsThatHandshakesStall) {
  const SelectedCurlHttpProvider insisted(
      HttpTransportSettings{
          .transport = HttpTransport::multi,
          .tls_handshake_blocks = true,
      },
      threading(), clock(), &log);
  EXPECT_EQ(insisted.transport(), HttpTransport::multi);
  EXPECT_EQ(log.count(LogSeverity::warning, "every new connection stalls"), 1U);
}

class CurlHttpDroppedConnections
    : public CurlHttpRig,
      public testing::WithParamInterface<HttpTransport> {};

TEST_P(CurlHttpDroppedConnections, ReconnectAndLogTheReason) {
  KeepAliveHttpServer server(2);
  ASSERT_TRUE(server.ready());
  RecordingDiagnostics log;
  SelectedCurlHttpProvider provider(
      HttpTransportSettings{.transport = GetParam()}, threading(), clock(),
      &log);
  ASSERT_EQ(provider.transport(), GetParam());
  EXPECT_TRUE(provider.perform(get(server.url("/first")), {}));
  provider.drop_connections("test reason");
  EXPECT_TRUE(provider.perform(get(server.url("/second")), {}));
  server.join();
  EXPECT_EQ(server.accepted(), 2U);
  EXPECT_EQ(log.count(LogSeverity::info, "dropped connections (test reason)"),
            1U);
}

INSTANTIATE_TEST_SUITE_P(
    EachTransport, CurlHttpDroppedConnections,
    testing::Values(HttpTransport::pooled, HttpTransport::multi),
    [](const testing::TestParamInfo<HttpTransport>& info) -> std::string {
      return info.param == HttpTransport::pooled ? "Pooled" : "Multi";
    });

}  // namespace
}  // namespace platform_curl_test
