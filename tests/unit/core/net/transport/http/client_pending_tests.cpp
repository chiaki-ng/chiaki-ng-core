#include "tests/unit/core/net/transport/http/client_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace http_test {
namespace {

using libchiaking::net::http::Client;
using libchiaking::net::http::Endpoint;
using libchiaking::net::http::Request;
using std::chrono::milliseconds;
using test_support::psn::Reply;
namespace platform = libchiaking::platform;

class StartingHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    platform::HttpResponse response;
    response.status_code = request.url.view().ends_with("/missing") ? 404 : 200;
    return response;
  }

  auto start(const platform::HttpRequest& request,
             const platform::StopToken& stop)
      -> platform::PendingHttpResponse override {
    started.emplace_back(request.url.view());
    return platform::PendingHttpResponse{
        std::make_unique<Answer>(*this, perform(request, stop))};
  }

  std::vector<std::string> started;
  std::vector<std::string> collected;

 private:
  class Answer final : public platform::PendingHttpResponse::Source {
   public:
    Answer(StartingHttp& owner,
           libchiaking::Expected<platform::HttpResponse, platform::HttpError>
               outcome)
        : owner_(owner), outcome_(std::move(outcome)) {}

    auto wait() -> libchiaking::Expected<platform::HttpResponse,
                                         platform::HttpError> override {
      owner_.collected.emplace_back(
          outcome_ ? std::to_string(outcome_.value().status_code) : "error");
      return std::move(outcome_);
    }

   private:
    StartingHttp& owner_;
    libchiaking::Expected<platform::HttpResponse, platform::HttpError> outcome_;
  };
};

class HttpClientStarted : public ::testing::Test {
 protected:
  StartingHttp starting;
  test_support::FakeLog log;
  std::shared_ptr<test_support::StopState> state =
      std::make_shared<test_support::StopState>();
  const platform::SteadyClock clock;
  Client client{starting, clock, platform::StopToken{state}, &log};
};

TEST_F(HttpClientStarted, RunsRequestsBeforeTheyAreCollected) {
  const auto missing = ClientRig::at("/missing");
  auto first = client.start(Request{ClientRig::plain, ClientRig::url});
  auto second = client.start(Request{ClientRig::plain, missing});
  EXPECT_EQ(starting.started,
            (std::vector<std::string>{std::string{ClientRig::url}, missing}));
  EXPECT_TRUE(starting.collected.empty());
  const auto later = std::move(second).wait();
  ASSERT_TRUE(later);
  EXPECT_EQ(later.value().status(), 404);
  EXPECT_EQ(later.value().attempts(), 1);
  const auto sooner = std::move(first).wait();
  ASSERT_TRUE(sooner);
  EXPECT_EQ(sooner.value().status(), 200);
  EXPECT_EQ(starting.collected, (std::vector<std::string>{"404", "200"}));
  EXPECT_TRUE(log.contains("[HTTP] test.plain ok HTTP 200 attempts=1"));
  EXPECT_TRUE(
      log.contains("[HTTP] test.plain client_error HTTP 404 attempts=1"));
}

TEST_F(HttpClientStarted, AbandonsARequestNobodyCollects) {
  {
    auto dropped = client.start(Request{ClientRig::plain, ClientRig::url});
    auto moved = std::move(dropped);
    const auto empty = std::move(dropped).wait();
    ASSERT_FALSE(empty);
    EXPECT_TRUE(empty.error().is_cancelled());
  }
  EXPECT_EQ(starting.started.size(), 1U);
  EXPECT_TRUE(starting.collected.empty());
  EXPECT_TRUE(log.contains("[HTTP] test.plain abandoned"));
}

TEST_F(HttpClientStarted, CancelsAStartWhenAlreadyStopped) {
  {
    auto dropped = client.start(Request{ClientRig::plain, ClientRig::url});
  }
  state->stopped = true;
  auto stopped = client.start(Request{ClientRig::plain, ClientRig::url});
  const auto cancelled = std::move(stopped).wait();
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is_cancelled());
  EXPECT_EQ(cancelled.error().attempts(), 0);
  EXPECT_EQ(starting.started.size(), 1U);
}

class HttpClientCollected : public ClientRig {};

TEST_F(HttpClientCollected, RetriesAsSendDoes) {
  fake.on("GET", std::string{url}, {Reply{503, ""}, Reply{200, "{}"}});
  constexpr Endpoint retried{
      .label = "test.retry",
      .retry = {.retries = 2, .delay = milliseconds{250}}};
  auto pending = client.start(Request{retried, url});
  const auto sent = std::move(pending).wait();
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value().status(), 200);
  EXPECT_EQ(sent.value().attempts(), 2);
  EXPECT_EQ(fake.seen.size(), 2U);
  EXPECT_EQ(waiter.waits, (std::vector<int>{250}));
  EXPECT_TRUE(
      log.contains("[HTTP] test.retry retry 1 after HTTP 503 in 250ms"));
  const auto again = std::move(pending).wait();
  ASSERT_FALSE(again);
  EXPECT_TRUE(again.error().is_cancelled());
}

}  // namespace
}  // namespace http_test
