#pragma once

#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {

class ControllerDeliverySink {
 public:
  enum class Kind : std::uint8_t { history, state };

  struct Call {
    Kind kind;
    std::uint8_t id;
    std::size_t events{};
  };

  using Event = libchiaking::net::protocol::takion::FeedbackHistoryEvent;

  auto post_controller_history_window(std::span<const Event> window,
                                      std::span<const std::uint8_t> seats,
                                      std::size_t fresh)
      -> libchiaking::Expected<void, int> {
    std::vector<Event> events(window.begin(), window.end());
    std::vector<std::uint8_t> ids(seats.begin(), seats.end());
    EXPECT_EQ(ids.size(), events.size());
    const std::uint8_t id = ids.empty() ? 0 : ids.front();
    if (fail_history) {
      fail_history = false;
      return libchiaking::unexpected(1);
    }
    if (full_history) {
      --full_history;
      return libchiaking::unexpected(1);
    }
    calls.push_back({Kind::history, id, events.size()});
    freshes.push_back(fresh);
    history_ids.push_back(std::move(ids));
    histories.push_back(events);
    last_history = std::move(events);
    return {};
  }

  auto post_controller_state(
      const libchiaking::net::protocol::remote_play::ControllerState& current)
      -> libchiaking::Expected<void, int> {
    if (fail_state) {
      fail_state = false;
      return libchiaking::unexpected(1);
    }
    calls.push_back({Kind::state, current.controller_id});
    last_state = current;
    return {};
  }

  std::vector<Call> calls;
  std::vector<std::vector<Event>> histories;
  std::vector<std::vector<std::uint8_t>> history_ids;
  std::vector<std::size_t> freshes;
  std::vector<Event> last_history;
  libchiaking::net::protocol::remote_play::ControllerState last_state;
  bool fail_history{};
  bool fail_state{};
  unsigned full_history{};
};

class ControllerDeliveryRig : public ::testing::Test {
 protected:
  using Sink = ControllerDeliverySink;
  using Kind = ControllerDeliverySink::Kind;
  using Delivery =
      libchiaking::net::protocol::remote_play::ControllerDelivery<Sink>;
  using State = libchiaking::net::protocol::remote_play::ControllerState;
  using Policy =
      libchiaking::net::protocol::remote_play::FeedbackDeliveryPolicy;
  using Button = libchiaking::net::protocol::remote_play::ControllerButton;
  using Digital =
      libchiaking::net::protocol::takion::FeedbackDigitalButtonEvent;
  using DigitalButton =
      libchiaking::net::protocol::takion::FeedbackDigitalButton;
  using Presence = libchiaking::net::protocol::takion::FeedbackPresenceEvent;
  using Analog = libchiaking::net::protocol::takion::FeedbackAnalogButtonEvent;
  using Touch = libchiaking::net::protocol::takion::FeedbackTouchEvent;

  static auto buttons(std::initializer_list<Button> pressed) -> std::uint32_t {
    std::uint32_t mask{};
    for (const auto button : pressed)
      mask |= static_cast<std::uint32_t>(button);
    return mask;
  }

  static auto call_is(const Sink& sink, std::size_t index, Kind kind)
      -> testing::AssertionResult {
    if (index >= sink.calls.size())
      return testing::AssertionFailure()
             << "only " << sink.calls.size() << " calls were made";
    if (sink.calls[index].kind != kind)
      return testing::AssertionFailure()
             << "call " << index << " posted "
             << (sink.calls[index].kind == Kind::history ? "a history"
                                                         : "a state");
    return testing::AssertionSuccess();
  }

  template <class Alternative>
  static auto event_at(const std::vector<Sink::Event>& events,
                       std::size_t index) -> const Alternative* {
    if (index >= events.size()) return nullptr;
    return std::get_if<Alternative>(&events[index]);
  }
};

}  // namespace remote_play_test
