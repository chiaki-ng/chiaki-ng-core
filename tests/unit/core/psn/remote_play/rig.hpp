#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/psn/remote_play/push_client.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/websocket.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {

class RealTime {
 public:
  static constexpr std::chrono::seconds patience{30};

  static auto now() -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{
        std::chrono::microseconds{clock.now_microseconds()}};
  }

  template <class Predicate>
  static auto eventually(Predicate predicate) -> testing::AssertionResult {
    const auto limit = now() + patience;
    while (!predicate() && now() < limit)
      sleeper.sleep_for(std::chrono::milliseconds{1});
    if (predicate()) return testing::AssertionSuccess();
    return testing::AssertionFailure()
           << "the condition did not hold within " << patience.count() << "s";
  }

  static void settle() { sleeper.sleep_for(std::chrono::milliseconds{20}); }

  static inline libchiaking::platform::SteadyClock clock;
  static inline libchiaking::platform::Sleeper sleeper{
      test_support::process_threading(), clock};
};

class TimedStop final : public libchiaking::platform::StopState {
 public:
  auto stop_requested() const noexcept -> bool override {
    return RealTime::now() >= limit;
  }

  static auto token() -> libchiaking::platform::StopToken {
    return libchiaking::platform::StopToken{std::make_shared<TimedStop>()};
  }

  std::chrono::steady_clock::time_point limit =
      RealTime::now() + RealTime::patience;
};

class ScriptedConnection final
    : public libchiaking::platform::WebSocketConnection {
 public:
  enum class Ending : std::uint8_t {
    cancelled,
    expire_cancelled,
    invalid,
    expire_valid
  };

  static constexpr std::string_view created_message =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","sessionId":"01234567-89ab-cdef-0123-456789abcdef","accountId":123456789})";

  auto receive(const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::WebSocketMessage,
                               libchiaking::platform::WebSocketError> override {
    ++calls;
    EXPECT_FALSE(stop.stop_requested()) << "receive called after a stop";
    if (script_index < script.size()) {
      if (clock != nullptr) clock->now_us += advance;
      return message(script[script_index++]);
    }
    switch (ending) {
      case Ending::expire_cancelled:
        clock->now_us = deadline;
        return libchiaking::unexpected(
            libchiaking::platform::WebSocketError::cancelled);
      case Ending::cancelled:
        if (parent != nullptr) parent->stopped = true;
        return libchiaking::unexpected(
            libchiaking::platform::WebSocketError::cancelled);
      case Ending::invalid:
        return libchiaking::unexpected(
            libchiaking::platform::WebSocketError::backend_failure);
      case Ending::expire_valid:
        clock->now_us = deadline;
        break;
    }
    return message(created_message);
  }

  auto close()
      -> libchiaking::Expected<void,
                               libchiaking::platform::WebSocketError> override {
    return {};
  }

  test_support::FakeClock* clock{};
  test_support::StopState* parent{};
  std::uint64_t deadline{};
  std::uint64_t advance{};
  Ending ending{Ending::cancelled};
  int calls{};
  std::vector<std::string_view> script;
  std::size_t script_index{};

 private:
  static auto message(std::string_view payload)
      -> libchiaking::platform::WebSocketMessage {
    libchiaking::platform::WebSocketMessage received{};
    received.payload.assign(payload);
    return received;
  }
};

class QueuedConnection final
    : public libchiaking::platform::WebSocketConnection {
 public:
  void push(std::string_view body) {
    libchiaking::platform::WebSocketMessage message;
    message.payload.assign(body);
    libchiaking::platform::Lock lock{mutex};
    messages.push_back(std::move(message));
    arrived.notify_all();
  }

  auto drained() -> bool {
    libchiaking::platform::Lock lock{mutex};
    return messages.empty();
  }

  auto receive(const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::WebSocketMessage,
                               libchiaking::platform::WebSocketError> override {
    libchiaking::platform::Lock lock{mutex};
    if (!arrived.wait(lock, stop, [&] { return !messages.empty(); }))
      return libchiaking::unexpected(
          libchiaking::platform::WebSocketError::cancelled);
    auto message = std::move(messages.front());
    messages.pop_front();
    return message;
  }

  auto close()
      -> libchiaking::Expected<void,
                               libchiaking::platform::WebSocketError> override {
    ++closes;
    return {};
  }

  unsigned closes{};
  libchiaking::platform::Mutex mutex{test_support::process_threading()};
  libchiaking::platform::ConditionVariable arrived{
      test_support::process_threading(), RealTime::clock};
  std::deque<libchiaking::platform::WebSocketMessage> messages;
};

class InterruptibleConnection final
    : public libchiaking::platform::WebSocketConnection {
 public:
  auto receive(const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::WebSocketMessage,
                               libchiaking::platform::WebSocketError> override {
    libchiaking::platform::Lock lock{mutex};
    entered = true;
    released_by_interrupt =
        woken.wait_for(lock, RealTime::patience, [&] { return interrupted; });
    if (stop.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::WebSocketError::cancelled);
    return libchiaking::unexpected(
        libchiaking::platform::WebSocketError::disconnected);
  }

  auto close()
      -> libchiaking::Expected<void,
                               libchiaking::platform::WebSocketError> override {
    return {};
  }

  void interrupt_receive() noexcept override {
    libchiaking::platform::Lock lock{mutex};
    interrupted = true;
    ++interrupts;
    woken.notify_all();
  }

  libchiaking::platform::Mutex mutex{test_support::process_threading()};
  libchiaking::platform::ConditionVariable woken{
      test_support::process_threading(), RealTime::clock};
  bool entered{};
  bool interrupted{};
  bool released_by_interrupt{};
  unsigned interrupts{};
};

class AcknowledgingHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    EXPECT_EQ(request.method, libchiaking::platform::HttpMethod::post);
    EXPECT_TRUE(request.url.view().ends_with("/sessionMessage"))
        << request.url.view();
    const std::string_view body{request.body.view()};
    EXPECT_NE(body.find("RESULT"), std::string_view::npos) << body;
    ++calls;
    if (block) {
      static_cast<void>(
          RealTime::sleeper.sleep_for(std::chrono::seconds{30}, stop));
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    }
    libchiaking::platform::HttpResponse response;
    response.status_code = fail ? 503 : 204;
    return response.clone();
  }

  std::atomic<unsigned> calls{};
  bool block{};
  bool fail{};
};

template <class Connection = QueuedConnection>
class ServiceRig {
 public:
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";

  [[nodiscard]] auto start() {
    return push.start_service(
        connection, session, platform.threading(), platform.clock(), 7,
        libchiaking::psn::auth::BearerToken{
            std::string_view{"SYNTHETIC_TEST_TOKEN"}},
        session_id, 12, {}, libchiaking::psn::rest::ConsoleFamily::ps5, {});
  }

  auto received_with(std::string_view fragment,
                     const libchiaking::platform::StopToken& stop = {})
      -> testing::AssertionResult {
    auto received = push.receive_notification(connection, stop);
    if (!received)
      return testing::AssertionFailure()
             << "no notification arrived while waiting for " << fragment;
    if (received.value().body().find(fragment) == std::string_view::npos)
      return testing::AssertionFailure()
             << "received " << received.value().body() << " without "
             << fragment;
    return testing::AssertionSuccess();
  }

  libchiaking::platform::DefaultPlatformFactory platform;
  test_support::RefusingWebSocket websocket{
      libchiaking::platform::WebSocketError::unsupported};
  Connection connection;
  AcknowledgingHttp http;
  libchiaking::psn::remote_play::SessionClient session{http, RealTime::clock,
                                                       platform.random()};
  libchiaking::psn::remote_play::PushClient push{websocket};
};

}  // namespace psn_test
