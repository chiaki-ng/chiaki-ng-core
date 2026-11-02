#pragma once

#include "src/core/psn/auth/app_token_slot.hpp"
#include "src/core/psn/session.hpp"
#include "src/core/psn/session_internals.hpp"
#include "tests/support/clock.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <chrono>
#include <cstdint>

namespace psn_test {

class SessionRig : public ClientRig {
 public:
  static constexpr std::uint64_t default_wall_start_us = 1'700'000'000'000'000;

  explicit SessionRig(std::uint64_t wall_start_us = default_wall_start_us)
      : wall{wall_start_us} {}

  static auto at(std::int64_t milliseconds)
      -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{milliseconds}};
  }

  auto internals() -> libchiaking::psn::SessionInternals {
    return libchiaking::psn::SessionInternals{session};
  }

  auto snapshot() -> libchiaking::psn::SessionSnapshot {
    return internals().snapshot();
  }

  auto app_token() -> libchiaking::psn::auth::AppTokenSlot& {
    return internals().app_token();
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  libchiaking::psn::Session session{
      libchiaking::psn::auth::Npsso{"NPSSO-SECRET"}, threading(), clock, wall};
};

}  // namespace psn_test
