#pragma once

#include "libchiaking/consoles.hpp"

#include <vector>

namespace test_support::consoles {

class StartStates final : public libchiaking::consoles::StartWatcher {
 public:
  void on_state(libchiaking::consoles::StartState state) override {
    states.push_back(state);
  }

  std::vector<libchiaking::consoles::StartState> states;
};

}  // namespace test_support::consoles
