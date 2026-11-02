#pragma once

#include "src/core/play/adapters/session/controllers_core.hpp"
#include "tests/unit/core/play/session_fakes.hpp"

#include <cstdint>
#include <string_view>

namespace play_test {

using test_support::LiveProviders;

class ControllersRig {
 public:
  using Core = libchiaking::play::Controllers::Core;
  using State = libchiaking::controllers::ControllerState;
  using SeatNumber = libchiaking::controllers::SeatNumber;

  static constexpr auto host = SeatNumber::first();

  explicit ControllersRig(Core::Waking waking = Core::Waking::every_sample)
      : queue_(LiveProviders::threading(), rig_.route, Core::queue_depth, waking) {}

  void start() { rig_.lifecycle.begin(); }

  auto queue() -> Core& { return queue_; }
  auto session() -> api_test::ScriptedSession& { return *rig_.session; }
  auto lifecycle() -> libchiaking::play::LockedSessionLifecycle& {
    return rig_.lifecycle;
  }
  auto logged(std::string_view text) const -> std::size_t {
    return rig_.logged(text);
  }

  static auto seat(std::uint8_t index) -> SeatNumber {
    return *SeatNumber::create(index);
  }

  static auto sample(std::uint32_t buttons) -> State {
    State state;
    state.buttons = buttons;
    return state;
  }

  static auto touched(std::int8_t id, std::uint16_t x, std::uint16_t y,
                      std::uint32_t buttons = 0) -> State {
    auto state = sample(buttons);
    state.touches[0] = {id, x, y};
    return state;
  }

 private:
  api_test::MemberRig rig_;
  Core queue_;
};

}  // namespace play_test
