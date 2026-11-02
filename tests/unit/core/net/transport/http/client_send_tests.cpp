#include "tests/unit/core/net/transport/http/client_rig.hpp"

#include "src/core/utils/json/json.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace http_client_send_shapes {

struct Small {
  std::int64_t a{};
};

}  // namespace http_client_send_shapes

namespace http_test {
namespace {

using http_client_send_shapes::Small;
using libchiaking::net::http::Client;
using libchiaking::net::http::default_connect_timeout;
using libchiaking::net::http::default_total_timeout;
using libchiaking::net::http::Endpoint;
using libchiaking::net::http::Failure;
using libchiaking::net::http::Method;
using libchiaking::net::http::Request;
using std::chrono::milliseconds;
using test_support::psn::Reply;
namespace platform = libchiaking::platform;

class HttpClientSend : public ClientRig {};

TEST_F(HttpClientSend, AppliesTheEndpointDefaults) {
  fake.on("GET", std::string{url}, {Reply{200, "{}"}});
  Request request{plain, url};
  request.authorize(TestBearer{"token-1"});
  const auto sent = client.send(std::move(request));
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().status(), 200);
  EXPECT_EQ(sent.value().attempts(), 1);
  ASSERT_EQ(fake.seen.size(), 1U);
  EXPECT_EQ(fake.seen[0].connect_timeout, default_connect_timeout);
  EXPECT_EQ(fake.seen[0].total_timeout, default_total_timeout);
  EXPECT_EQ(fake.seen[0].header("Authorization"), "Bearer token-1");
}

TEST_F(HttpClientSend, CapsTheConnectTimeoutAtAShortTotalTimeout) {
  fake.on("POST", std::string{url}, {Reply{204, ""}});
  constexpr Endpoint quick{.label = "test.quick",
                           .method = Method::post,
                           .total_timeout = milliseconds{2500}};
  Request request{quick, url};
  request.json(R"({"a":1})");
  EXPECT_TRUE(client.send(std::move(request)));
  ASSERT_EQ(fake.seen.size(), 1U);
  EXPECT_EQ(fake.seen[0].connect_timeout, milliseconds{2500});
  EXPECT_EQ(fake.seen[0].body, R"({"a":1})");
  EXPECT_EQ(fake.seen[0].header("Content-Type"), "application/json");
}

TEST_F(HttpClientSend, LetsAnEndpointLengthenTheConnectTimeout) {
  fake.on("GET", std::string{url}, {Reply{200, "{}"}});
  constexpr Endpoint slow{.label = "test.slow",
                          .connect_timeout = std::chrono::seconds{45},
                          .total_timeout = std::chrono::seconds{45}};
  EXPECT_TRUE(client.send(Request{slow, url}));
  ASSERT_EQ(fake.seen.size(), 1U);
  EXPECT_EQ(fake.seen[0].connect_timeout, std::chrono::seconds{45});
}

TEST_F(HttpClientSend, ReturnsErrorStatusesAsResponses) {
  fake.on("GET", std::string{url},
          {Reply{401, ""}, Reply{403, ""}, Reply{503, ""}, Reply{404, ""}});
  for (const auto expected : {401, 403, 503, 404}) {
    const auto sent = client.send(Request{plain, url});
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value().status(), expected);
  }
}

TEST_F(HttpClientSend, ReadsHeadersCaseInsensitively) {
  Reply response{302, ""};
  response.headers.push_back({"location", "https://example.invalid/?code=abc"});
  response.headers.push_back({"Set-Cookie", "JSESSIONID=xyz"});
  fake.on("GET", std::string{url}, {response});
  const auto sent = client.send(Request{plain, url});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().header("Location"),
            "https://example.invalid/?code=abc");
  EXPECT_EQ(sent.value().header("set-cookie"), "JSESSIONID=xyz");
  EXPECT_FALSE(sent.value().header("X-Missing"));
  EXPECT_EQ(sent.value().headers().size(), 2U);
}

TEST_F(HttpClientSend, ReportsATransportFailureWithoutKeepingIt) {
  fake.on("GET", std::string{url}, {Reply::transport()});
  const auto failed = client.send(Request{plain, url});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind(), Failure::Kind::transport);
  EXPECT_EQ(failed.error().error(), platform::HttpError::disconnected);
  EXPECT_EQ(failed.error().attempts(), 1);
  EXPECT_EQ(failed.error().name(), "disconnected");
  fake.on("GET", at("/ok"), {Reply{200, "{}"}});
  EXPECT_TRUE(client.send(Request{plain, at("/ok")}));
}

TEST_F(HttpClientSend, ParsesAJsonResponse) {
  fake.on("GET", std::string{url},
          {Reply{200, R"({"a":1})"}, Reply{200, "not json"}});
  const auto good = client.send(Request{plain, url});
  ASSERT_TRUE(good);
  const auto document = good.value().document<Small>();
  ASSERT_TRUE(document);
  EXPECT_EQ(document.value().a, 1);
  const auto bad = client.send(Request{plain, url});
  ASSERT_TRUE(bad);
  EXPECT_EQ(bad.value().document<Small>().error(),
            libchiaking::json::ParseError::invalid_json);
}

class HttpClientStop : public ClientRig {};

TEST_F(HttpClientStop, CancelsWithoutSendingWhenStoppedFirst) {
  state->stopped = true;
  const auto sent = client.send(Request{plain, url});
  ASSERT_FALSE(sent);
  EXPECT_TRUE(sent.error().is_cancelled());
  EXPECT_EQ(sent.error().attempts(), 0);
  EXPECT_TRUE(fake.seen.empty());
  EXPECT_TRUE(log.contains("[HTTP] test.plain cancelled attempts=0 0ms"));
}

TEST_F(HttpClientStop, DiscardsTheReplyWhenStoppedDuringTheRequest) {
  Reply response{200, "{}"};
  response.during = [stopping = state] { stopping->stopped = true; };
  fake.on("GET", std::string{url}, {response});
  const auto sent = client.send(Request{plain, url});
  ASSERT_FALSE(sent);
  EXPECT_TRUE(sent.error().is_cancelled());
  EXPECT_FALSE(sent.error().error());
}

TEST_F(HttpClientStop, KeepsTheReplyWhenTheEndpointAsks) {
  Reply response{201, "{}"};
  response.during = [stopping = state] { stopping->stopped = true; };
  fake.on("GET", std::string{url}, {response});
  constexpr Endpoint kept{.label = "test.kept", .keep_reply_after_stop = true};
  const auto sent = client.send(Request{kept, url});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().status(), 201);
  EXPECT_EQ(sent.value().body(), "{}");
}

TEST_F(HttpClientStop, WaitsThroughTheWaiterUntilStopped) {
  test_support::psn::FakeHttp idle;
  auto waiting_state = std::make_shared<test_support::StopState>();
  const platform::StopToken waiting_stop{waiting_state};
  const platform::SteadyClock steady;
  Client without{idle, steady, waiting_stop};
  EXPECT_TRUE(without.wait(milliseconds{100}));
  test_support::FakeWaiter counted;
  Client waiting{idle, steady, waiting_stop, nullptr, &counted};
  EXPECT_TRUE(waiting.wait(milliseconds{100}));
  EXPECT_TRUE(waiting.wait(milliseconds{0}));
  ASSERT_EQ(counted.waits.size(), 1U);
  EXPECT_EQ(counted.waits[0], 100);
  counted.stop_state = waiting_state.get();
  counted.stop_on_wait = 2;
  EXPECT_FALSE(waiting.wait(milliseconds{50}));
  EXPECT_FALSE(waiting.wait(milliseconds{50}));
  EXPECT_TRUE(waiting.stop_requested());
}

class HttpClientRetry : public ClientRig {};

TEST_F(HttpClientRetry, RetriesServerErrorsAndTooManyRequests) {
  fake.on("GET", std::string{url},
          {Reply{503, ""}, Reply{429, ""}, Reply{200, "{}"}});
  constexpr Endpoint retried{
      .label = "test.retry",
      .retry = {.retries = 3, .delay = milliseconds{250}}};
  const auto sent = client.send(Request{retried, url});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().status(), 200);
  EXPECT_EQ(sent.value().attempts(), 3);
  EXPECT_EQ(fake.seen.size(), 3U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{250, 250}));
  EXPECT_TRUE(
      log.contains("[HTTP] test.retry retry 1 after HTTP 503 in 250ms"));
  EXPECT_TRUE(
      log.contains("[HTTP] test.retry retry 2 after HTTP 429 in 250ms"));
  EXPECT_TRUE(log.contains("[HTTP] test.retry ok HTTP 200 attempts=3"));
}

TEST_F(HttpClientRetry, SkipsAReasonThePolicyLeavesOut) {
  fake.on("GET", std::string{url}, {Reply::transport(), Reply{200, "{}"}});
  constexpr Endpoint strict{.label = "test.strict",
                            .retry = {2, milliseconds{0}, true, true, false}};
  const auto sent = client.send(Request{strict, url});
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error().attempts(), 1);
  EXPECT_EQ(sent.error().error(), platform::HttpError::disconnected);
}

TEST_F(HttpClientRetry, StopsAtTheRetryCap) {
  fake.on("GET", std::string{url}, {Reply{500, ""}});
  constexpr Endpoint greedy{.label = "test.greedy", .retry = {.retries = 1000}};
  const auto capped = client.send(Request{greedy, url});
  ASSERT_TRUE(capped);
  EXPECT_EQ(capped.value().status(), 500);
  EXPECT_EQ(capped.value().attempts(), libchiaking::net::http::max_retries + 1);
}

TEST_F(HttpClientRetry, StopsWhenCancelledDuringTheDelay) {
  fake.on("GET", std::string{url}, {Reply{500, ""}});
  waiter.stop_state = state.get();
  waiter.stop_on_wait = 1;
  constexpr Endpoint patient{
      .label = "test.patient",
      .retry = {.retries = 3, .delay = milliseconds{100}}};
  const auto cancelled = client.send(Request{patient, url});
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is_cancelled());
  EXPECT_EQ(cancelled.error().attempts(), 1);
  EXPECT_FALSE(cancelled.error().error());
}

class RecordingHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    label = std::string{request.label};
    maximum = request.maximum_response_bytes;
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = 200;
    response.body.assign(body);
    return response;
  }

  std::string label;
  std::size_t maximum{};
  std::string body;
  std::optional<platform::HttpError> error;
};

class HttpClientLimits : public ::testing::Test {
 protected:
  static constexpr Endpoint capped{.label = "test.labelled",
                                   .maximum_response_bytes = 8};

  RecordingHttp recording;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  const platform::SteadyClock clock;
  Client client{recording, clock, platform::StopToken{state}};
};

TEST_F(HttpClientLimits, PassesTheLabelAndSizeCapToTheProvider) {
  recording.body = R"({"long":"document"})";
  const auto sent = client.send(Request{capped, ClientRig::url});
  ASSERT_TRUE(sent);
  EXPECT_EQ(recording.label, "test.labelled");
  EXPECT_EQ(recording.maximum, 8U);
}

TEST_F(HttpClientLimits, RefusesToParseABodyOverTheCap) {
  recording.body = R"({"long":"document"})";
  const auto sent = client.send(Request{capped, ClientRig::url});
  ASSERT_TRUE(sent);
  const auto parsed = sent.value().document<glz::raw_json>();
  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error(), libchiaking::json::ParseError::too_large);
}

TEST_F(HttpClientLimits, ReportsAnOversizedReplyAsTooLarge) {
  recording.error = platform::HttpError::response_too_large;
  const auto refused = client.send(Request{capped, ClientRig::url});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind(), Failure::Kind::too_large);
  EXPECT_EQ(refused.error().name(), "too_large");
}

}  // namespace
}  // namespace http_test
