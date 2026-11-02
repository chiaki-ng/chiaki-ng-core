#pragma once

#include <libchiaking/platform/http/curl/multi_provider.hpp>
#include <libchiaking/platform/http/curl/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace platform_curl_test {

using namespace libchiaking::platform;

enum class CurlHttpKind { pooled, multi, multi_without_extras };

class CurlHttpRig : public testing::Test {
 public:
  static std::string kind_name(
      const testing::TestParamInfo<CurlHttpKind>& info) {
    switch (info.param) {
      case CurlHttpKind::pooled:
        return "Pooled";
      case CurlHttpKind::multi:
        return "Multi";
      case CurlHttpKind::multi_without_extras:
        return "MultiWithoutExtras";
    }
    return "Unknown";
  }

 protected:
  static DefaultThreadingProvider& threading() {
    return test_support::process_threading();
  }

  static SteadyClock& clock() {
    static SteadyClock steady;
    return steady;
  }

  static std::chrono::microseconds now() {
    return std::chrono::microseconds{
        static_cast<std::int64_t>(clock().now_microseconds())};
  }

  static void pause(std::chrono::nanoseconds duration) {
    Sleeper sleeper{threading(), clock()};
    static_cast<void>(sleeper.sleep_for(duration));
  }

  static CurlCapabilities without_extras() {
    auto capabilities = CurlCapabilities::detect();
    capabilities.wakeup = false;
    capabilities.asynchronous_dns = false;
    capabilities.http2 = false;
    capabilities.asynchronous_tls_handshake = false;
    return capabilities;
  }

  static std::unique_ptr<HttpProvider> make_provider(CurlHttpKind kind) {
    if (kind == CurlHttpKind::pooled)
      return std::make_unique<CurlHttpProvider>(threading());
    const auto capabilities = kind == CurlHttpKind::multi_without_extras
                                  ? without_extras()
                                  : CurlCapabilities::detect();
    auto provider = CurlMultiHttpProvider::create(
        CurlMultiSettings{}, capabilities, threading(), clock(), nullptr);
    if (!provider) return nullptr;
    return std::move(provider).value();
  }

  static HttpRequest get(const std::string& url) {
    HttpRequest request;
    request.url = libchiaking::SecretString{url};
    return request;
  }

  static std::size_t count_occurrences(std::string_view text,
                                       std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t position = text.find(needle);
         position != std::string_view::npos;
         position = text.find(needle, position + needle.size()))
      ++count;
    return count;
  }
};

class CurlHttpProviders : public CurlHttpRig,
                          public testing::WithParamInterface<CurlHttpKind> {
 protected:
  void SetUp() override {
    provider_ = make_provider(GetParam());
    ASSERT_NE(provider_, nullptr);
  }

  HttpProvider& provider() { return *provider_; }

 private:
  std::unique_ptr<HttpProvider> provider_;
};

}  // namespace platform_curl_test
