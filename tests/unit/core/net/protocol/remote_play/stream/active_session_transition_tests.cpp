#include "tests/unit/core/net/protocol/remote_play/stream/active_session_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

class ActiveSessionTransitions : public ::testing::Test {
 protected:
  using Step = std::function<std::string(ActiveSessionRig&)>;

  static auto outcome(
      const libchiaking::Expected<
          void, libchiaking::net::protocol::remote_play::ActiveSessionError>&
          result) -> std::string {
    return result ? std::string{"ok"}
                  : std::format("e{}", static_cast<int>(result.error()));
  }

  static auto trace(ActiveSessionRig& rig) -> std::string {
    auto& owner = *rig.owner;
    std::size_t media = 0;
    while (owner.take_media_event()) ++media;
    std::size_t feedback = 0;
    while (owner.take_feedback_event()) ++feedback;
    const auto now = rig.clock.now_microseconds();
    const auto relative = [now](std::optional<std::uint64_t> at) {
      return at ? static_cast<std::int64_t>(*at) -
                      static_cast<std::int64_t>(now)
                : std::int64_t{-1};
    };
    return std::format(
        " |s{} d{} w{} m{} f{} c{} x{} fail{}", static_cast<int>(owner.state()),
        relative(owner.next_deadline_us()), relative(owner.next_wake_us()),
        media, feedback, rig.state->close_count, rig.state->sent.size(),
        owner.failure() ? static_cast<int>(owner.failure()->cause) : -1);
  }

  static auto rows() -> std::vector<std::pair<std::string_view, Step>> {
    return {
        {"started",
         [](ActiveSessionRig& rig) {
           rig.start();
           return std::string{};
         }},
        {"handshaking",
         [](ActiveSessionRig& rig) {
           rig.start();
           rig.answer_handshake();
           return outcome(rig.owner->pump_once());
         }},
        {"refused_receive",
         [](ActiveSessionRig& rig) {
           rig.start();
           rig.state->failed_receives = 1;
           return outcome(rig.owner->pump_once());
         }},
        {"lost",
         [](ActiveSessionRig& rig) {
           rig.state->fail_send = true;
           rig.start();
           rig.state->fail_send = false;
           return outcome(rig.owner->pump_once());
         }},
        {"ready",
         [](ActiveSessionRig& rig) {
           rig.make_ready();
           return std::string{};
         }},
        {"stopped",
         [](ActiveSessionRig& rig) {
           rig.make_ready();
           return outcome(rig.owner->request_stop());
         }},
        {"stopped_early",
         [](ActiveSessionRig& rig) {
           rig.start();
           return outcome(rig.owner->request_stop());
         }},
        {"disconnected_ready",
         [](ActiveSessionRig& rig) {
           rig.make_ready();
           rig.enqueue_video(0, H264Bits::frame(true));
           rig.enqueue_control(takion::Disconnect{"bye", std::nullopt});
           return outcome(rig.owner->pump_once());
         }},
        {"broken_ready",
         [](ActiveSessionRig& rig) {
           rig.make_ready();
           rig.enqueue_video(0, H264Bits::frame(true));
           rig.state->failed_receives = 1;
           return outcome(rig.owner->pump_once());
         }},
    };
  }

  static auto columns() -> std::vector<std::pair<std::string_view, Step>> {
    return {
        {"nothing", [](ActiveSessionRig&) { return std::string{}; }},
        {"pump",
         [](ActiveSessionRig& rig) { return outcome(rig.owner->pump_once()); }},
        {"stop",
         [](ActiveSessionRig& rig) {
           return outcome(rig.owner->request_stop());
         }},
        {"later",
         [](ActiveSessionRig& rig) {
           rig.clock.advance_ms(3'000);
           return outcome(rig.owner->pump_once());
         }},
        {"much_later",
         [](ActiveSessionRig& rig) {
           rig.clock.advance_ms(60'000);
           const auto first = outcome(rig.owner->pump_once());
           return first + " " + outcome(rig.owner->pump_once());
         }},
        {"idr",
         [](ActiveSessionRig& rig) {
           return outcome(rig.owner->post_idr_request());
         }},
    };
  }

  static auto pins()
      -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {
        {"started.nothing", "/ |s0 d200000 w100000 m0 f0 c0 x1 fail-1"},
        {"started.pump", "/ok |s0 d200000 w100000 m0 f0 c0 x1 fail-1"},
        {"started.stop", "/ok |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"started.later", "/ok |s0 d200000 w100000 m0 f0 c0 x3 fail-1"},
        {"started.much_later", "/e7 e7 |s9 d-1 w-1 m0 f0 c1 x1 fail7"},
        {"started.idr", "/e5 |s0 d200000 w100000 m0 f0 c0 x1 fail-1"},
        {"handshaking.nothing", "ok/ |s2 d200000 w200000 m0 f0 c0 x3 fail-1"},
        {"handshaking.pump", "ok/ok |s2 d200000 w200000 m0 f0 c0 x3 fail-1"},
        {"handshaking.stop", "ok/ok |s8 d-1 w-1 m0 f0 c1 x4 fail-1"},
        {"handshaking.later", "ok/ok |s2 d200000 w200000 m0 f0 c0 x5 fail-1"},
        {"handshaking.much_later", "ok/e7 e7 |s9 d-1 w-1 m0 f0 c1 x4 fail7"},
        {"handshaking.idr", "ok/e5 |s2 d200000 w200000 m0 f0 c0 x3 fail-1"},
        {"refused_receive.nothing",
         "ok/ |s0 d200000 w100000 m0 f0 c0 x2 fail-1"},
        {"refused_receive.pump",
         "ok/ok |s0 d200000 w100000 m0 f0 c0 x2 fail-1"},
        {"refused_receive.stop", "ok/ok |s8 d-1 w-1 m0 f0 c1 x2 fail-1"},
        {"refused_receive.later",
         "ok/ok |s0 d200000 w100000 m0 f0 c0 x4 fail-1"},
        {"refused_receive.much_later",
         "ok/e7 e7 |s9 d-1 w-1 m0 f0 c1 x2 fail7"},
        {"refused_receive.idr", "ok/e5 |s0 d200000 w100000 m0 f0 c0 x2 fail-1"},
        {"lost.nothing", "ok/ |s0 d5000000 w5000000 m0 f0 c1 x0 fail-1"},
        {"lost.pump", "ok/ok |s0 d5000000 w5000000 m0 f0 c1 x0 fail-1"},
        {"lost.stop", "ok/ok |s8 d-1 w-1 m0 f0 c1 x0 fail-1"},
        {"lost.later", "ok/ok |s0 d2000000 w2000000 m0 f0 c1 x0 fail-1"},
        {"lost.much_later", "ok/e7 e7 |s9 d-1 w-1 m0 f0 c1 x0 fail7"},
        {"lost.idr", "ok/e5 |s0 d5000000 w5000000 m0 f0 c1 x0 fail-1"},
        {"ready.nothing", "/ |s7 d200000 w200000 m0 f0 c0 x8 fail-1"},
        {"ready.pump", "/ok |s7 d200000 w200000 m0 f0 c0 x8 fail-1"},
        {"ready.stop", "/ok |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"ready.later", "/ok |s7 d200000 w200000 m0 f0 c0 x10 fail-1"},
        {"ready.much_later", "/ok ok |s7 d200000 w200000 m0 f0 c0 x10 fail-1"},
        {"ready.idr", "/ok |s7 d200000 w0 m0 f0 c0 x8 fail-1"},
        {"stopped.nothing", "ok/ |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped.pump", "ok/e1 |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped.stop", "ok/ok |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped.later", "ok/e1 |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped.much_later", "ok/e1 e1 |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped.idr", "ok/e5 |s8 d-1 w-1 m0 f0 c1 x9 fail-1"},
        {"stopped_early.nothing", "ok/ |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"stopped_early.pump", "ok/e1 |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"stopped_early.stop", "ok/ok |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"stopped_early.later", "ok/e1 |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"stopped_early.much_later", "ok/e1 e1 |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"stopped_early.idr", "ok/e5 |s8 d-1 w-1 m0 f0 c1 x1 fail-1"},
        {"disconnected_ready.nothing", "e8/ |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"disconnected_ready.pump", "e8/e8 |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"disconnected_ready.stop", "e8/ok |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"disconnected_ready.later", "e8/e8 |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"disconnected_ready.much_later",
         "e8/e8 e8 |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"disconnected_ready.idr", "e8/e5 |s9 d-1 w-1 m2 f0 c1 x10 fail8"},
        {"broken_ready.nothing", "e1/ |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
        {"broken_ready.pump", "e1/e1 |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
        {"broken_ready.stop", "e1/ok |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
        {"broken_ready.later", "e1/e1 |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
        {"broken_ready.much_later", "e1/e1 e1 |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
        {"broken_ready.idr", "e1/e5 |s9 d-1 w-1 m0 f0 c1 x8 fail1"},
    };
  }
};

TEST_F(ActiveSessionTransitions, EveryCallInEveryStateDoesWhatTheTableSays) {
  const auto table = pins();
  std::size_t matched = 0;
  for (const auto& [row, setup] : rows()) {
    for (const auto& [column, step] : columns()) {
      ActiveSessionRig rig;
      const auto before = setup(rig);
      if (!rig.owner) {
        ADD_FAILURE() << row << " did not start";
        continue;
      }
      const auto actual = before + "/" + step(rig) + trace(rig);
      const auto name = std::string{row} + "." + std::string{column};
      const auto pin = std::ranges::find_if(
          table, [&](const auto& item) { return item.first == name; });
      if (pin == table.end()) {
        ADD_FAILURE() << "{\"" << name << "\", \"" << actual << "\"},";
        continue;
      }
      ++matched;
      EXPECT_EQ(actual, pin->second) << name;
    }
  }
  EXPECT_EQ(matched, table.size());
}

}  // namespace
}  // namespace remote_play_test
