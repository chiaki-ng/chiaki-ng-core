#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace platform_curl_test {
namespace {

class CurlHttpConnections : public CurlHttpProviders {};

TEST_P(CurlHttpConnections, ReuseOneConnectionForSequentialRequests) {
  KeepAliveHttpServer server(3);
  ASSERT_TRUE(server.ready());
  for (int index = 0; index < 3; ++index) {
    const auto result = provider().perform(get(server.url("/reuse")), {});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().status_code, 200);
  }
  server.join();
  EXPECT_EQ(server.requests().size(), 3U);
  EXPECT_EQ(server.accepted(), 1U);
}

TEST_P(CurlHttpConnections, CompleteConcurrentRequestsOnAtMostOnePerCaller) {
  constexpr std::size_t thread_count = 4;
  constexpr std::size_t per_thread = 5;
  KeepAliveHttpServer server(thread_count * per_thread);
  ASSERT_TRUE(server.ready());
  auto& shared = provider();
  Mutex mutex{threading()};
  std::size_t succeeded = 0;
  std::vector<Thread> threads;
  for (std::size_t thread = 0; thread < thread_count; ++thread) {
    auto started = Thread::start(
        threading(), "concurrent caller", ThreadRole::unspecified, [&] {
          for (std::size_t index = 0; index < per_thread; ++index) {
            const auto result =
                shared.perform(get(server.url("/concurrent")), {});
            if (result && result.value().status_code == 200) {
              const Lock lock{mutex};
              ++succeeded;
            }
          }
        });
    ASSERT_TRUE(started);
    threads.push_back(std::move(started).value());
  }
  for (auto& thread : threads) static_cast<void>(thread.join());
  server.join();
  EXPECT_EQ(succeeded, thread_count * per_thread);
  EXPECT_LE(server.accepted(), thread_count);
}

INSTANTIATE_TEST_SUITE_P(EveryTransport, CurlHttpConnections,
                         testing::Values(CurlHttpKind::pooled,
                                         CurlHttpKind::multi,
                                         CurlHttpKind::multi_without_extras),
                         CurlHttpRig::kind_name);

}  // namespace
}  // namespace platform_curl_test
