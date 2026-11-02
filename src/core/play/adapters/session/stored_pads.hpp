/// @file
/// @brief Declares StoredPads, the last controller state the couch protocol
/// holds for each pad of a stream.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <array>
#include <cstdint>

namespace libchiaking::play {

/// @brief The last controller state stored for each pad, numbered by pad,
/// which a pad starts from when the console admits it.
///
/// Owned by the console half of the couch protocol and used on its thread; it
/// is not synchronised.
class StoredPads {
 public:
  /// @brief Builds the store with every pad's state numbered by its pad.
  StoredPads() noexcept {
    for (std::uint8_t pad = 0; pad < states_.size(); ++pad)
      states_[pad].controller_id = pad;
  }

  /// @brief Reads a pad's stored state.
  ///
  /// @param pad The pad number, below Pad::count.
  /// @return The last stored state.
  auto at(std::uint8_t pad) const noexcept
      -> const net::protocol::remote_play::ControllerState& {
    return states_[pad];
  }

  /// @brief Stores a pad's state, by its controller id; an id out of range is
  /// ignored.
  ///
  /// @param state The state.
  void store(
      const net::protocol::remote_play::ControllerState& state) noexcept {
    if (state.controller_id < states_.size())
      states_[state.controller_id] = state;
  }

  /// @brief Stores a state once it passes validation.
  ///
  /// @param state The state.
  /// @return Nothing; StreamError::controller_rejected for an invalid state.
  [[nodiscard]] auto store_valid(
      const net::protocol::remote_play::ControllerState& state)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!net::protocol::remote_play::ControllerSample::validate(
            net::protocol::remote_play::ControllerSample::sanitize_motion(
                state)))
      return unexpected(
          net::protocol::remote_play::StreamError::controller_rejected);
    store(state);
    return {};
  }

 private:
  /// @brief The last state stored per pad.
  std::array<net::protocol::remote_play::ControllerState,
             net::protocol::remote_play::Pad::count>
      states_{};
};

}  // namespace libchiaking::play
