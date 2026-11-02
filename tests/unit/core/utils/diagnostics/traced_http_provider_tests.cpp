#include "libchiaking/detail/diagnostics/api_capture.hpp"
#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/diagnostics/traced_http_provider.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::ApiCapture;
using libchiaking::diagnostics::LogQueue;
using libchiaking::diagnostics::TracedHttpProvider;

class TracedHttp : public ::testing::Test {
 protected:
  static auto secret_request() -> libchiaking::platform::HttpRequest {
    libchiaking::platform::HttpRequest request;
    request.url = libchiaking::SecretString{"https://SYNTHETIC_SECRET_REQUEST"};
    request.headers.push_back(
        {"Authorization", libchiaking::SecretString{"SYNTHETIC_SECRET_TOKEN"}});
    return request;
  }

  auto logged(const char* text) -> bool {
    bool seen = false;
    while (const auto record = queue.read())
      seen = seen || std::strstr(record->c_str(), text);
    return seen;
  }

  const libchiaking::platform::SteadyClock monotonic;
  LogQueue queue{monotonic};
  RefusingHttp http;
  TracedHttpProvider observed{http, queue, monotonic};
  libchiaking::platform::HttpRequest request = secret_request();
};

TEST_F(TracedHttp, LogsTheStatusOfACallWithoutItsSecrets) {
  const auto response = observed.perform(request, {});
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().status_code, 403);
  bool saw_status = false;
  while (const auto record = queue.read()) {
    EXPECT_FALSE(std::strstr(record->c_str(), "SYNTHETIC")) << record->c_str();
    EXPECT_FALSE(std::strstr(record->c_str(), "https://")) << record->c_str();
    saw_status =
        saw_status || (std::strstr(record->c_str(), "status=403") &&
                       std::strstr(record->c_str(), "received_bytes=2"));
  }
  EXPECT_TRUE(saw_status);
}

TEST_F(TracedHttp, LogsAStartedCallWhenItIsCollected) {
  auto pending = observed.start(request, {});
  const auto begun = queue.read();
  ASSERT_TRUE(begun);
  EXPECT_TRUE(std::strstr(begun->c_str(), "outcome=begin")) << begun->c_str();
  EXPECT_FALSE(queue.read());
  const auto response = std::move(pending).wait();
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().status_code, 403);
  EXPECT_TRUE(logged("outcome=response status=403"));
}

TEST_F(TracedHttp, LogsADroppedCallAsCancelled) {
  {
    auto dropped = observed.start(request, {});
  }
  EXPECT_TRUE(logged("outcome=cancelled"));
}

TEST_F(TracedHttp, CapturesTheRequestAndResponseOfACall) {
  ApiCapture capture;
  capture.enable(true);
  request.url = libchiaking::SecretString{
      "https://web.np.playstation.com/api/sessionManager/v1/"
      "remotePlaySessions/SECRET/sessionMessage"};
  request.label = "psn.session_manager.send_session_message";
  const std::array request_body{std::byte{0}, std::byte{255}, std::byte{1},
                                std::byte{2}};
  request.body =
      libchiaking::SecretBytes{std::span<const std::byte>{request_body}};
  TracedHttpProvider capturing{http, queue, monotonic, &capture, 9};
  const auto response = capturing.perform(request, {});
  ASSERT_TRUE(response);
  EXPECT_EQ(response.value().reason, "SYNTHETIC_SECRET_REASON");
  const auto outgoing = Captures::take(capture);
  EXPECT_NE(outgoing.find("\"attempt\":9,\"call\":1,\"phase\":\"request\""),
            std::string::npos)
      << outgoing;
  EXPECT_NE(outgoing.find("PSN Send session signalling message"),
            std::string::npos);
  EXPECT_NE(outgoing.find("\"body_base64\":\"AP8BAg==\""), std::string::npos);
  EXPECT_EQ(outgoing.find("SYNTHETIC_SECRET_TOKEN"), std::string::npos);
  EXPECT_NE(outgoing.find("[redacted]"), std::string::npos);
  const auto incoming = Captures::take(capture);
  EXPECT_NE(incoming.find("\"call\":1,\"phase\":\"response\""),
            std::string::npos)
      << incoming;
  EXPECT_NE(incoming.find("\"status\":403"), std::string::npos);
  EXPECT_NE(incoming.find("\"body_base64\":\"AQI=\""), std::string::npos);
  EXPECT_EQ(capture.read({}), 0U);
  while (const auto record = queue.read())
    EXPECT_FALSE(std::strstr(record->c_str(), "SECRET")) << record->c_str();
}

}  // namespace
}  // namespace diagnostics_test
