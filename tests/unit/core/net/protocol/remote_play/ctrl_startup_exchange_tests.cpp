#include "tests/support/clock.hpp"
#include "tests/unit/core/net/protocol/remote_play/ctrl_loopback_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::ConsoleExchangeError;

static_assert(
    !std::is_copy_constructible_v<remote_play::CtrlStartupConnection>);

class ExchangeCrypto final : public platform::CryptoProvider {
 public:
  using Result = libchiaking::Expected<void, platform::CryptoError>;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> digest) -> Result override {
    std::ranges::fill(digest, std::byte{0});
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output) -> Result override {
    std::ranges::fill(output, std::byte{0});
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
};

class CtrlStartupExchange : public CtrlLoopbackRig {
 protected:
  void SetUp() override { ASSERT_TRUE(listen(listener, endpoint)); }

  auto request_for(std::uint16_t port)
      -> std::optional<remote_play::CtrlStartupRequest> {
    remote_play::AuthMaterial material;
    const std::array<std::byte, 16> key{};
    const std::array<std::byte, 32> did{};
    auto encoded = remote_play::CtrlCodec::encode_startup_request(
        crypto, remote_play::RegistrationTarget::ps5_1, material, "127.0.0.1",
        port, key, did, remote_play::VideoCodec::h264);
    EXPECT_TRUE(encoded);
    if (!encoded) return std::nullopt;
    return std::move(encoded.value());
  }

  auto begin() -> testing::AssertionResult {
    auto request = request_for(endpoint.port);
    if (!request) return testing::AssertionFailure() << "no request";
    expected_request = std::string(request->view());
    if (!exchange.begin(endpoint, std::move(*request)))
      return testing::AssertionFailure() << "the exchange did not begin";
    return testing::AssertionSuccess();
  }

  auto accept_until() -> std::unique_ptr<Socket> {
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (still_before(deadline)) {
      const auto progress = exchange.pump_once();
      EXPECT_TRUE(progress);
      if (!progress) return nullptr;
      EXPECT_FALSE(progress.value());
      auto accepted = listener->accept();
      if (accepted) return std::move(accepted.value());
      EXPECT_EQ(accepted.error(), NetError::would_block);
      nap();
    }
    ADD_FAILURE() << "no connection arrived";
    return nullptr;
  }

  void read_exact_request(Socket& accepted) {
    std::vector<std::byte> received;
    const auto deadline = deadline_after(std::chrono::seconds{2});
    while (received.size() < expected_request.size() &&
           still_before(deadline)) {
      const auto progress = exchange.pump_once();
      ASSERT_TRUE(progress);
      ASSERT_FALSE(progress.value());
      std::array<std::byte, 1024> buffer{};
      const auto part = accepted.receive(buffer);
      if (part)
        received.insert(
            received.end(), buffer.begin(),
            buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
      else
        ASSERT_EQ(part.error(), NetError::would_block);
      nap();
    }
    ASSERT_EQ(received, owned(expected_request));
  }

  void expect_startup_error(std::string_view response_text, bool close_peer,
                            ConsoleExchangeError expected_error) {
    ASSERT_TRUE(begin());
    auto accepted = accept_until();
    ASSERT_NE(accepted, nullptr);
    const auto deadline = deadline_after(std::chrono::seconds{2});
    bool request_received = false;
    while (!request_received && still_before(deadline)) {
      const auto progress = exchange.pump_once();
      ASSERT_TRUE(progress);
      ASSERT_FALSE(progress.value());
      std::array<std::byte, 1024> buffer{};
      const auto part = accepted->receive(buffer);
      if (part && part.value() > 0)
        request_received = true;
      else if (!part) {
        ASSERT_EQ(part.error(), NetError::would_block);
      }
      nap();
    }
    ASSERT_TRUE(request_received);
    ASSERT_NO_FATAL_FAILURE(send_all(*accepted, bytes(response_text)));
    if (close_peer) {
      EXPECT_TRUE(accepted->close());
    }
    bool failed = false;
    while (!failed && still_before(deadline)) {
      const auto progress = exchange.pump_once();
      if (!progress) {
        EXPECT_EQ(progress.error(), expected_error);
        failed = true;
      } else {
        EXPECT_FALSE(progress.value());
      }
      nap();
    }
    ASSERT_TRUE(failed);
    const auto failure = exchange.last_failure_progress();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->error, expected_error);
    EXPECT_EQ(failure->response_bytes_received, response_text.size());
    EXPECT_GT(failure->request_bytes_sent, 0U);
    EXPECT_FALSE(failure->retried_header_timeout);
    EXPECT_FALSE(exchange.take());
    EXPECT_TRUE(exchange.cancel());
    EXPECT_TRUE(exchange.last_failure_progress());
    if (!close_peer) {
      EXPECT_TRUE(accepted->close());
    }
    EXPECT_TRUE(listener->close());
  }

  void expect_header_timeout_retry(bool second_responds) {
    ASSERT_TRUE(begin());
    auto first = accept_until();
    ASSERT_NE(first, nullptr);
    ASSERT_NO_FATAL_FAILURE(read_exact_request(*first));
    clock.now_us +=
        static_cast<std::uint64_t>(remote_play::rp_phase_timeout.count());
    const auto retry = exchange.pump_once();
    ASSERT_TRUE(retry);
    EXPECT_FALSE(retry.value());
    EXPECT_FALSE(exchange.last_failure_progress());
    EXPECT_TRUE(first->close());
    auto second = accept_until();
    ASSERT_NE(second, nullptr);
    ASSERT_NO_FATAL_FAILURE(read_exact_request(*second));
    if (second_responds) {
      ASSERT_NO_FATAL_FAILURE(
          send_all(*second, bytes("HTTP/1.1 200 OK\r\n\r\n")));
      const auto deadline = deadline_after(std::chrono::seconds{2});
      bool done = false;
      while (!done && still_before(deadline)) {
        const auto progress = exchange.pump_once();
        ASSERT_TRUE(progress);
        done = progress.value();
        nap();
      }
      ASSERT_TRUE(done);
      auto connection = exchange.take();
      ASSERT_TRUE(connection);
      EXPECT_EQ(connection.value().next_local_crypt_counter, 5U);
      EXPECT_FALSE(exchange.last_failure_progress());
      EXPECT_TRUE(connection.value().socket->close());
    } else {
      clock.now_us +=
          static_cast<std::uint64_t>(remote_play::rp_phase_timeout.count());
      const auto expired = exchange.pump_once();
      ASSERT_FALSE(expired);
      EXPECT_EQ(expired.error(), ConsoleExchangeError::timeout);
      const auto failure = exchange.last_failure_progress();
      ASSERT_TRUE(failure);
      EXPECT_TRUE(failure->retried_header_timeout);
      EXPECT_EQ(failure->response_bytes_received, 0U);
      EXPECT_FALSE(exchange.take());
    }
    EXPECT_TRUE(exchange.cancel());
    EXPECT_TRUE(second->close());
    EXPECT_TRUE(listener->close());
  }

  std::unique_ptr<Listener> listener;
  platform::SocketEndpoint endpoint;
  test_support::FakeClock clock{1'000'000};
  ExchangeCrypto crypto;
  remote_play::CtrlStartupExchange exchange{network.sockets(), clock};
  std::string expected_request;
};

TEST_F(CtrlStartupExchange, ReportsARefusedConnectDistinctly) {
  std::unique_ptr<Listener> closed;
  platform::SocketEndpoint closed_endpoint;
  ASSERT_TRUE(listen(closed, closed_endpoint, false));
  EXPECT_TRUE(closed->close());
  auto request = request_for(closed_endpoint.port);
  ASSERT_TRUE(request);
  const auto begun = exchange.begin(closed_endpoint, std::move(*request));
  if (!begun) {
    EXPECT_EQ(begun.error(), ConsoleExchangeError::connection_refused);
    return;
  }
  const auto deadline = deadline_after(std::chrono::seconds{2});
  while (still_before(deadline)) {
    const auto progress = exchange.pump_once();
    if (!progress) {
      EXPECT_EQ(progress.error(), ConsoleExchangeError::connection_refused);
      return;
    }
    EXPECT_FALSE(progress.value());
    nap();
  }
  ADD_FAILURE() << "the refused connect was never reported";
}

TEST_F(CtrlStartupExchange, HandsOffTheSocketWithTheFirstFrameUnread) {
  ASSERT_TRUE(begin());
  auto accepted = accept_until();
  ASSERT_NE(accepted, nullptr);
  ASSERT_NO_FATAL_FAILURE(read_exact_request(*accepted));
  const auto first_frame = remote_play::CtrlCodec::encode_frame_header(0x33, 0);
  ASSERT_TRUE(first_frame);
  auto response = owned("HTTP/1.1 200 OK\r\n\r\n");
  response.insert(response.end(), first_frame.value().begin(),
                  first_frame.value().end());
  ASSERT_NO_FATAL_FAILURE(send_all(*accepted, response));
  sleep(std::chrono::milliseconds{50});
  const auto whole = exchange.pump_once();
  ASSERT_TRUE(whole);
  EXPECT_TRUE(whole.value());
  auto connection = exchange.take();
  ASSERT_TRUE(connection);
  EXPECT_EQ(connection.value().next_local_crypt_counter, 5U);
  EXPECT_FALSE(exchange.take());
  EXPECT_TRUE(exchange.cancel());
  EXPECT_EQ(receive_exact(*connection.value().socket, 8),
            std::vector<std::byte>(first_frame.value().begin(),
                                   first_frame.value().end()));
  EXPECT_TRUE(connection.value().socket->close());
  EXPECT_TRUE(accepted->close());
  EXPECT_TRUE(listener->close());
}

TEST_F(CtrlStartupExchange, ReportsARejectedRequest) {
  expect_startup_error("HTTP/1.1 403 Forbidden\r\n\r\n", false,
                       ConsoleExchangeError::refused);
}

TEST_F(CtrlStartupExchange, ReportsAResponseCutShortAsTruncated) {
  expect_startup_error("HTTP/1.1 200 OK\r\nRP-Server-Type: AAE", true,
                       ConsoleExchangeError::truncated);
}

TEST_F(CtrlStartupExchange, ReportsAPeerThatClosesWithoutAnsweringAsTruncated) {
  expect_startup_error("", true, ConsoleExchangeError::truncated);
}

TEST_F(CtrlStartupExchange, ReportsAnOversizedResponse) {
  expect_startup_error(std::string(512, 'x'), false,
                       ConsoleExchangeError::too_large);
}

TEST_F(CtrlStartupExchange, TimesOutAtThePhaseDeadlineAndCancelsTwice) {
  ASSERT_TRUE(begin());
  clock.now_us +=
      static_cast<std::uint64_t>(remote_play::rp_phase_timeout.count());
  const auto expired = exchange.pump_once();
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error(), ConsoleExchangeError::timeout);
  EXPECT_TRUE(exchange.cancel());
  EXPECT_TRUE(exchange.cancel());
  EXPECT_TRUE(exchange.last_failure_progress());
  EXPECT_FALSE(exchange.take());
  EXPECT_TRUE(listener->close());
}

TEST_F(CtrlStartupExchange, AnExplicitCancelLeavesNoFailureProgress) {
  ASSERT_TRUE(begin());
  EXPECT_TRUE(exchange.cancel());
  EXPECT_FALSE(exchange.last_failure_progress());
  EXPECT_TRUE(listener->close());
}

TEST_F(CtrlStartupExchange, RetriesAHeaderTimeoutOnceAndSucceeds) {
  expect_header_timeout_retry(true);
}

TEST_F(CtrlStartupExchange, FailsWhenTheRetriedRequestAlsoTimesOut) {
  expect_header_timeout_retry(false);
}

}  // namespace
}  // namespace remote_play_test
