#include "tests/unit/platform/http/curl/keep_alive_http_server.hpp"
#include "tests/unit/platform/http/curl/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace platform_curl_test {
namespace {

class CurlHttpPool : public CurlHttpRig {
 protected:
  static std::optional<std::size_t> connections_for_alternating_hosts(
      const CurlPoolSettings& settings) {
    KeepAliveHttpServer first(2);
    KeepAliveHttpServer second(2);
    if (!first.ready() || !second.ready()) return std::nullopt;
    CurlHttpProvider provider(threading(), settings);
    for (const auto* server : {&first, &second, &first, &second}) {
      const auto result = provider.perform(get(server->url("/alternate")), {});
      if (!result || result.value().status_code != 200) return std::nullopt;
    }
    first.join();
    second.join();
    return first.accepted() + second.accepted();
  }
};

TEST_F(CurlHttpPool, KeepsOneConnectionPerHostByDefault) {
  const auto connections = connections_for_alternating_hosts({});
  ASSERT_TRUE(connections.has_value());
  EXPECT_EQ(*connections, 2U);
}

TEST_F(CurlHttpPool, ReconnectsWhenEachHandleKeepsOneConnection) {
  const auto connections = connections_for_alternating_hosts(
      {.idle_handles = 8, .connections_per_handle = 1});
  ASSERT_TRUE(connections.has_value());
  EXPECT_GE(*connections, 3U);
}

TEST_F(CurlHttpPool, KeepsAtMostItsIdleHandles) {
  constexpr std::size_t thread_count = 4;
  KeepAliveHttpServer server(thread_count);
  ASSERT_TRUE(server.ready());
  CurlHttpProvider provider(threading(),
                            {.idle_handles = 1, .connections_per_handle = 1});
  std::vector<Thread> threads;
  for (std::size_t thread = 0; thread < thread_count; ++thread) {
    auto started =
        Thread::start(threading(), "pool caller", ThreadRole::unspecified, [&] {
          static_cast<void>(provider.perform(get(server.url("/idle")), {}));
        });
    ASSERT_TRUE(started);
    threads.push_back(std::move(started).value());
  }
  for (auto& thread : threads) static_cast<void>(thread.join());
  server.join();
  EXPECT_LE(provider.drop_connections(), 1U);
}

}  // namespace
}  // namespace platform_curl_test
