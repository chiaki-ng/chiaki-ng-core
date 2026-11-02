#pragma once

#include "libchiaking/detail/users/account_id.hpp"
#include "src/core/play/adapters/session/console_commands.hpp"
#include "tests/unit/core/play/session_fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace play_test {

using test_support::LiveProviders;

class ConsoleRig {
 public:
  using Venue = libchiaking::play::Venue;
  using Generation = libchiaking::catalog::ServiceGeneration;
  using StreamState = libchiaking::net::protocol::remote_play::StreamState;
  using Microphone = libchiaking::play::Microphone;

  explicit ConsoleRig(std::optional<Venue> kind = Venue::home(),
                      bool running = true,
                      libchiaking::play::CommandSettings settings = {})
      : rig_(kind),
        queue_(LiveProviders::threading(), rig_.route),
        seats_(LiveProviders::threading(), rig_.route, queue_,
               libchiaking::controllers::ControllerKind::dualsense),
        commands_(LiveProviders::threading(), rig_.route, rig_.lifecycle,
                  rig_.clock, seats_, settings) {
    if (running) rig_.lifecycle.begin();
  }

  static auto cloud(Generation generation = Generation::ps5_cloud) -> Venue {
    return Venue::cloud(generation);
  }

  auto streaming() -> libchiaking::play::CommandService {
    return commands_.service(StreamState::streaming,
                             std::chrono::milliseconds{8});
  }

  auto negotiating() -> libchiaking::play::CommandService {
    return commands_.service(StreamState::negotiating_stream,
                             std::chrono::milliseconds{8});
  }

  auto session() -> api_test::ScriptedSession& { return *rig_.session; }
  auto commands() -> libchiaking::play::ConsoleCommands& { return commands_; }
  auto seats() -> libchiaking::play::Seats::Core& { return seats_; }
  auto queue() -> libchiaking::play::Controllers::Core& { return queue_; }
  auto clock() -> api_test::ManualClock& { return rig_.clock; }
  auto lifecycle() -> libchiaking::play::LockedSessionLifecycle& {
    return rig_.lifecycle;
  }
  auto logged(std::string_view text) const -> std::size_t {
    return rig_.logged(text);
  }

  static auto seat(std::uint8_t index) -> libchiaking::controllers::SeatNumber {
    return *libchiaking::controllers::SeatNumber::create(index);
  }

  static auto code(std::string_view digits) -> libchiaking::play::AccessCode {
    return *libchiaking::play::AccessCode::parse(digits);
  }

  template <class Command>
  static auto index_of() -> std::size_t {
    return libchiaking::net::protocol::remote_play::StreamCommand{Command{}}
        .index();
  }

  template <class Result>
  static auto refused(const Result& result,
                      libchiaking::play::InputRefusal refusal)
      -> testing::AssertionResult {
    if (result) return testing::AssertionFailure() << "it was accepted";
    if (result.error() != refusal)
      return testing::AssertionFailure()
             << "it was refused as " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  static auto microphone_frame(int value)
      -> std::array<std::byte, Microphone::frame_size> {
    std::array<std::byte, Microphone::frame_size> frame{};
    frame.fill(static_cast<std::byte>(value));
    return frame;
  }

 private:
  api_test::MemberRig rig_;
  libchiaking::play::Controllers::Core queue_;
  libchiaking::play::Seats::Core seats_;
  libchiaking::play::ConsoleCommands commands_;
};

}  // namespace play_test
