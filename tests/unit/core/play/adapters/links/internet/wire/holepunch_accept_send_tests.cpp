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
#include <string_view>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace psn_remote_play = libchiaking::psn::remote_play;
using SendErrorKind = internet::holepunch::AcceptSendErrorKind;

class AcceptPost final : public platform::HttpProvider {
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
    EXPECT_EQ(request.headers[0].name, "Authorization");
    EXPECT_EQ(request.headers[0].value.view(), "Bearer SYNTHETIC_TEST_TOKEN");
    const auto body = std::string_view{request.body.view()};
    EXPECT_NE(body.find(R"(\"action\":\"ACCEPT\",\"reqId\":7,\"error\":0)"),
              std::string_view::npos);
    EXPECT_NE(body.find(R"(\"sid\":1,\"peerSid\":2)"), std::string_view::npos);
    EXPECT_NE(body.find(R"(\"mappedAddr\":\"203.0.113.7\")"),
              std::string_view::npos);
    if (error) return libchiaking::unexpected(*error);
    platform::HttpResponse response;
    response.status_code = 204;
    return response;
  }

  std::optional<platform::HttpError> error;
  unsigned calls{};
};

class HolepunchAcceptSend : public ::testing::Test {
 protected:
  auto send(std::uint64_t attempt, std::uint64_t req_id,
            const platform::StopToken& stop = {}) {
    return internet::holepunch::Accept::send(
        attempt, 7, req_id, 1, 2, selected_, local_, remote_, client_,
        libchiaking::psn::auth::BearerToken{
            std::string_view{"SYNTHETIC_TEST_TOKEN"}},
        "01234567-89ab-cdef-0123-456789abcdef", 12, uid_,
        libchiaking::psn::rest::ConsoleFamily::ps5, stop);
  }

  auto send_prepared(std::uint64_t attempt, std::uint64_t req_id,
                     const platform::StopToken& stop = {}) {
    return internet::holepunch::Accept::send_request(
        attempt, 7, req_id, prepared_, client_,
        libchiaking::psn::auth::BearerToken{
            std::string_view{"SYNTHETIC_TEST_TOKEN"}},
        "01234567-89ab-cdef-0123-456789abcdef", 12, uid_,
        libchiaking::psn::rest::ConsoleFamily::ps5, stop);
  }

  static constexpr std::array<std::byte, 32> uid_{};
  test_support::FixedRandom random_{std::byte{}};
  AcceptPost http_;
  psn_remote_play::SessionClient client_{
      http_, test_support::LiveProviders::steady(), random_};
  const nat::Candidate selected_{nat::CandidateType::local, "192.168.1.42", "",
                                 12345, 0};
  const nat::Candidate local_{nat::CandidateType::local, "203.0.113.7", "wrong",
                              54321, 1};
  const nat::Candidate remote_{nat::CandidateType::stun, "198.51.100.9", "",
                               40000, 0};
  const nat::ConnRequest prepared_ = internet::holepunch::Accept::build_request(
      1, 2, selected_, local_, remote_);
};

TEST_F(HolepunchAcceptSend, PostsTheAcceptForTheCurrentAttempt) {
  EXPECT_TRUE(send(7, 7).has_value());
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptSend, PostsAPreparedRequestForTheCurrentAttempt) {
  EXPECT_TRUE(send_prepared(7, 7).has_value());
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptSend, RefusesAStalePreparedAttemptWithoutPosting) {
  for (const auto attempt : {0U, 6U}) {
    SCOPED_TRACE(attempt);
    const auto stale = send_prepared(attempt, 7);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().kind(), SendErrorKind::stale_attempt);
  }
  EXPECT_EQ(http_.calls, 0U);
}

TEST_F(HolepunchAcceptSend, RefusesAnInvalidPreparedRequestIdWithoutPosting) {
  for (const auto req_id : {0U, 65536U}) {
    SCOPED_TRACE(req_id);
    const auto invalid = send_prepared(7, req_id);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().kind(), SendErrorKind::invalid_req_id);
  }
  EXPECT_EQ(http_.calls, 0U);
}

TEST_F(HolepunchAcceptSend, RefusesAStaleAttemptWithoutPosting) {
  for (const auto attempt : {0U, 6U}) {
    SCOPED_TRACE(attempt);
    const auto stale = send(attempt, 7);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().kind(), SendErrorKind::stale_attempt);
  }
  EXPECT_EQ(http_.calls, 0U);
}

TEST_F(HolepunchAcceptSend, RefusesAnInvalidRequestIdWithoutPosting) {
  for (const auto req_id : {0U, 65536U}) {
    SCOPED_TRACE(req_id);
    const auto invalid = send(7, req_id);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().kind(), SendErrorKind::invalid_req_id);
  }
  EXPECT_EQ(http_.calls, 0U);
}

TEST_F(HolepunchAcceptSend, ReportsATransportFailure) {
  http_.error = platform::HttpError::timed_out;
  const auto timeout = send_prepared(7, 7);
  ASSERT_FALSE(timeout);
  EXPECT_EQ(timeout.error().kind(), SendErrorKind::session_failure);
  EXPECT_EQ(timeout.error().session()->kind,
            psn_remote_play::SessionErrorKind::transport_failure);
  EXPECT_EQ(timeout.error().session()->transport, platform::HttpError::timed_out);
  EXPECT_EQ(http_.calls, 1U);
}

TEST_F(HolepunchAcceptSend, ReportsACancelledSendWithoutPosting) {
  const auto stopped =
      platform::StopToken{std::make_shared<test_support::Stopped>()};
  const auto cancelled = send_prepared(7, 7, stopped);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().kind(), SendErrorKind::session_failure);
  EXPECT_EQ(cancelled.error().session()->kind,
            psn_remote_play::SessionErrorKind::cancelled);
  EXPECT_EQ(http_.calls, 0U);
}

}  // namespace
}  // namespace play_test
