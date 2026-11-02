#pragma once

#include "src/core/play/adapters/session/seats_console.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {

class RecordingActive {
 public:
  using Log = std::vector<std::string>;

  auto post_controller_history_window(
      std::span<const libchiaking::net::protocol::takion::FeedbackHistoryEvent>
          events,
      std::span<const std::uint8_t> ids, std::size_t)
      -> libchiaking::Expected<void, int> {
    EXPECT_EQ(events.size(), ids.size());
    std::string entry = "history:";
    for (std::size_t index = 0; index < ids.size(); ++index) {
      if (index) entry += ",";
      entry += std::to_string(ids[index]);
    }
    log_.push_back(entry);
    return {};
  }

  auto post_controller_state(
      const libchiaking::net::protocol::remote_play::ControllerState& state)
      -> libchiaking::Expected<void, int> {
    log_.push_back("state" + std::to_string(state.controller_id));
    return {};
  }

  void set_pad_controller_type(std::uint8_t pad, std::uint8_t type) {
    log_.push_back("type" + std::to_string(pad) + "=" + std::to_string(type));
  }

  auto announce_pad(std::uint8_t pad, bool uses_id)
      -> libchiaking::Expected<void, int> {
    if (fail_announce_) return libchiaking::unexpected(1);
    log_.push_back("announce" + std::to_string(pad) + (uses_id ? "id" : ""));
    return {};
  }

  auto announce_pads(std::uint8_t session_count)
      -> libchiaking::Expected<void, int> {
    log_.push_back("announce_all" + std::to_string(session_count));
    return {};
  }

  auto disconnect_pad(std::uint8_t pad) -> libchiaking::Expected<void, int> {
    log_.push_back("disconnect" + std::to_string(pad));
    return {};
  }

  void admit_pad(std::uint8_t pad, bool publish_held) {
    log_.push_back("admit" + std::to_string(pad) + (publish_held ? "" : "q"));
  }

  void release_pad(std::uint8_t pad) {
    log_.push_back("release" + std::to_string(pad));
  }

  auto pad_led(std::uint8_t pad) const -> std::array<std::uint8_t, 3> {
    return {static_cast<std::uint8_t>(pad * 10U), 1, 2};
  }

  auto log() const -> const Log& { return log_; }
  void forget() { log_.clear(); }
  void fail_announcing(bool fail) { fail_announce_ = fail; }

 private:
  bool fail_announce_{};
  Log log_;
};

class ConsoleSeatsTest : public testing::Test {
 protected:
  using Couch = libchiaking::play::Seats::Core::Console<RecordingActive>;
  using Kind = libchiaking::play::Seats::Core::ConsoleKind;
  using SeatState = libchiaking::play::Seats::Core::ConsoleSeatState;
  using Delivery = libchiaking::net::protocol::remote_play::ControllerDelivery<
      RecordingActive>;
  using Ctrl = libchiaking::net::protocol::remote_play::CtrlFeatures;
  using Codec = libchiaking::net::protocol::remote_play::CtrlCodec;
  using Frame = libchiaking::net::protocol::remote_play::CtrlOutboundFrame;
  using Event = libchiaking::net::protocol::remote_play::StreamEvent;
  using Error = libchiaking::net::protocol::remote_play::StreamError;
  using Log = RecordingActive::Log;
  using Types = std::vector<std::uint16_t>;

  static auto account(std::string_view base64) -> libchiaking::AccountId {
    return libchiaking::AccountId::from_base64(base64).value();
  }

  static auto access_code(std::string_view digits)
      -> libchiaking::play::AccessCode {
    return libchiaking::play::AccessCode::parse(digits).value();
  }

  static auto drain(Ctrl& ctrl) -> std::vector<Frame> {
    std::vector<Frame> frames;
    while (const auto* frame = ctrl.front_outbound()) {
      frames.push_back(*frame);
      ctrl.pop_outbound();
    }
    return frames;
  }

  static auto frame_types(Ctrl& ctrl) -> Types {
    Types types;
    for (const auto& frame : drain(ctrl)) types.push_back(frame.type);
    return types;
  }

  static auto notice(Ctrl& ctrl, Couch::Binding& binding, std::uint16_t type,
                     std::vector<std::byte> payload) -> std::vector<Event> {
    std::vector<Event> events;
    auto decoded = Codec::decode_notice(type, payload);
    EXPECT_TRUE(decoded);
    if (!decoded) return events;
    ctrl.on_notice(std::move(*decoded), events, &binding);
    return events;
  }

  template <class Wanted>
  static auto event_at(const std::vector<Event>& events, std::size_t index)
      -> const Wanted* {
    return index < events.size() ? std::get_if<Wanted>(&events[index])
                                 : nullptr;
  }

  template <class Result>
  static auto refused(const Result& result, Error error)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "it was accepted";
    if (result.error() != error)
      return testing::AssertionFailure()
             << "it was refused as " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }
};

}  // namespace play_test
