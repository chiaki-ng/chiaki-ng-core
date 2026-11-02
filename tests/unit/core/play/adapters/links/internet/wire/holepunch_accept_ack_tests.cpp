#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using AckErrorKind = internet::holepunch::AcceptAckErrorKind;

class AckPost final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_FALSE(stop.stop_requested());
    EXPECT_EQ(request.method, platform::HttpMethod::post);
    EXPECT_EQ(request.url.view(),
              "https://web.np.playstation.com/api/sessionManager/v1/"
              "remotePlaySessions/01234567-89ab-cdef-0123-456789abcdef/"
              "sessionMessage");
    const auto body = std::string_view{request.body.view()};
    const std::string expected_req =
        "\\\"reqId\\\":" + std::to_string(req_id) + ",";
    EXPECT_NE(body.find(expected_req), std::string_view::npos);
    EXPECT_EQ(
        body.substr(0, body.find("reqId")),
        "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, body="
        "{\\\"action\\\":\\\"RESULT\\\",\\\"");
    EXPECT_EQ(
        body.substr(body.find("\\\"error")),
        "\\\"error\\\":0,"
        "\\\"connRequest\\\":{}}\",\"to\":[{\"accountId\":\"12\","
        "\"deviceUniqueId\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"platform\":\"PS5\"}]}");
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = 204;
    return response;
  }

  std::optional<platform::HttpError> error;
  unsigned calls{};
  unsigned req_id{7};
};

class HolepunchAcceptAck : public ::testing::Test {
 protected:
  auto send(internet::holepunch::AckPeerAccept ack, std::uint64_t attempt,
            const platform::StopToken& stop = {}) {
    return internet::holepunch::Accept::send_ack(
        ack, attempt, client_,
        libchiaking::psn::auth::BearerToken{
            std::string_view{"SYNTHETIC_TEST_TOKEN"}},
        "01234567-89ab-cdef-0123-456789abcdef", 12, uid_,
        libchiaking::psn::rest::ConsoleFamily::ps5, stop);
  }

  static constexpr std::array<std::byte, 32> uid_{};
  test_support::FixedRandom random_{std::byte{}};
  AckPost http_;
  psn_remote_play::SessionClient client_{
      http_, test_support::LiveProviders::steady(), random_};
};

TEST_F(HolepunchAcceptAck, SendsTheAcknowledgementTheWaitAskedFor) {
  internet::holepunch::AcceptWait wait;
  ASSERT_TRUE(wait.begin(5));
  nat::SessionMessage accept{};
  accept.action = nat::Action::accept;
  accept.req_id = 7;
  wait.on_message(5, accept);
  const auto ack = wait.take_ack();
  ASSERT_TRUE(ack);
  EXPECT_TRUE(send(*ack, 5));
  EXPECT_EQ(wait.state(), internet::holepunch::AcceptWaitState::ack_pending);
  EXPECT_TRUE(wait.ack_sent(5));
  EXPECT_EQ(wait.state(), internet::holepunch::AcceptWaitState::ready);
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptAck, RefusesAStaleAttemptWithoutSending) {
  for (const auto effect : {internet::holepunch::AckPeerAccept{0, 7},
                            internet::holepunch::AckPeerAccept{4, 7}}) {
    SCOPED_TRACE(effect.attempt);
    const auto stale = send(effect, 5);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().kind(), AckErrorKind::stale_attempt);
  }
  EXPECT_EQ(http_.calls, 0U);
}

TEST_F(HolepunchAcceptAck, SendsAZeroRequestId) {
  http_.req_id = 0;
  EXPECT_TRUE(send({5, 0}, 5));
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptAck, ReportsATransportFailure) {
  http_.error = platform::HttpError::timed_out;
  const auto timeout = send({5, 7}, 5);
  ASSERT_FALSE(timeout);
  EXPECT_EQ(timeout.error().kind(), AckErrorKind::session_failure);
  EXPECT_EQ(timeout.error().session()->kind,
            psn_remote_play::SessionErrorKind::transport_failure);
  EXPECT_EQ(timeout.error().session()->transport, platform::HttpError::timed_out);
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptAck, ReportsACancelledSendWithoutPosting) {
  const auto stopped =
      platform::StopToken{std::make_shared<test_support::Stopped>()};
  const auto cancelled = send({5, 7}, 5, stopped);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind(), AckErrorKind::session_failure);
  EXPECT_EQ(cancelled.error().session()->kind,
            psn_remote_play::SessionErrorKind::cancelled);
  EXPECT_EQ(http_.calls, 0U);
}

}  // namespace
}  // namespace play_test
