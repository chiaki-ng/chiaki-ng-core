#include "tests/unit/platform/http/curl/rig.hpp"
#include "tests/unit/platform/http/curl/silent_http_server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <utility>

namespace platform_curl_test {
namespace {

using namespace std::chrono_literals;

class CurlMultiCancel : public CurlHttpProviders {};

TEST_P(CurlMultiCancel, StopsASilentRequestPromptly) {
  SilentHttpServer server;
  ASSERT_TRUE(server.ready());
  StopSource stop;
  auto canceller =
      Thread::start(threading(), "canceller", ThreadRole::unspecified, [&stop] {
        pause(200ms);
        stop.request_stop();
      });
  ASSERT_TRUE(canceller);
  const auto started = now();
  const auto result =
      provider().perform(get(server.url("/silent")), stop.token());
  const auto elapsed = now() - started;
  static_cast<void>(canceller.value().join());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpError::cancelled);
  EXPECT_LE(elapsed, 600ms);
}

INSTANTIATE_TEST_SUITE_P(EveryMultiTransport, CurlMultiCancel,
                         testing::Values(CurlHttpKind::multi,
                                         CurlHttpKind::multi_without_extras),
                         CurlHttpRig::kind_name);

class CurlMultiShutdown : public CurlHttpRig {};

TEST_F(CurlMultiShutdown, CancelsRequestsStillRunning) {
  SilentHttpServer server;
  ASSERT_TRUE(server.ready());
  auto created = CurlMultiHttpProvider::create(CurlMultiSettings{}, threading(),
                                               clock(), nullptr);
  ASSERT_TRUE(created);
  auto provider = std::move(created).value();
  auto* const running = provider.get();
  std::optional<HttpError> error;
  auto caller = Thread::start(threading(), "caller", ThreadRole::unspecified,
                              [&error, running, &server] {
                                const auto result = running->perform(
                                    get(server.url("/silent")), {});
                                if (!result) error = result.error();
                              });
  ASSERT_TRUE(caller);
  pause(200ms);
  provider.reset();
  static_cast<void>(caller.value().join());
  EXPECT_EQ(error, HttpError::cancelled);
}

}  // namespace
}  // namespace platform_curl_test
