/// @file
/// @brief Declares DrivenLink, the calls that run a link: start, pump, wait,
/// wake and stop.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <chrono>
#include <string_view>

namespace libchiaking::play {

class LinkEventSink;

/// @brief The calls that run a link, made by whoever holds its turn: the
/// link driver for a session, a reconnecting link for its current attempt.
///
/// The holder sets the event sink, starts the link, pumps it once per loop
/// pass, waits on it and stops it, on its own thread; any thread may wake it.
class DrivenLink {
 public:
  /// @brief The result of a call: nothing, or the stream error.
  using Result = Expected<void, net::protocol::remote_play::StreamError>;

  /// @brief Allows destruction through the interface.
  virtual ~DrivenLink() = default;

  /// @brief Starts connecting; blocks while the link reaches the other end.
  ///
  /// @return Nothing once started; the error otherwise.
  [[nodiscard]] virtual auto start() -> Result = 0;
  /// @brief Does one round of the link's work: receives, sends and advances
  /// its state.
  ///
  /// @return Nothing; the error otherwise.
  [[nodiscard]] virtual auto pump_once() -> Result = 0;
  /// @brief Blocks until the link has work or the time passes.
  ///
  /// @param timeout The longest wait.
  /// @return Nothing; the error otherwise.
  [[nodiscard]] virtual auto wait(std::chrono::milliseconds timeout)
      -> Result = 0;
  /// @brief Ends a wait early; may be called from any thread.
  virtual void wake() noexcept = 0;
  /// @brief Passes the player's datacenter pick to a cloud launch.
  ///
  /// @param name The datacenter name.
  /// @return Nothing when the launch took it; the error otherwise.
  [[nodiscard]] virtual auto select_datacenter(std::string_view name)
      -> Result = 0;
  /// @brief Stops the link, closing the stream.
  ///
  /// @return Nothing; the error otherwise.
  [[nodiscard]] virtual auto stop() -> Result = 0;
  /// @brief Stops the link because the host is going to sleep.
  ///
  /// @return Nothing; the error otherwise.
  [[nodiscard]] virtual auto stop_for_sleep() -> Result = 0;
  /// @brief Sets where the link's events go.
  ///
  /// @param sink The receiver; must outlive the link.
  virtual void set_event_sink(LinkEventSink& sink) noexcept = 0;
};

}  // namespace libchiaking::play
