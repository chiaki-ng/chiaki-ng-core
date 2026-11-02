/// @file
/// @brief Declares RestRequest, the request that puts the console into rest
/// mode as a session ends.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/session/command_route.hpp"

#include <chrono>
#include <cstdint>

namespace libchiaking::play {

/// @brief Asking the console to go to rest mode, and holding the session open
/// until the request is delivered.
///
/// A request is queued, sent by the session loop, and then flushed for
/// RestRequest::default_flush (or the configured time) before the session
/// ends. Only a console session can rest; a refused request ends the session
/// without rest. Every member locks one mutex. Owned by ConsoleCommands; the
/// route, lifecycle and clock must outlive it.
class RestRequest {
 public:
  /// @brief How long the session runs after the request was sent, by default.
  static constexpr std::chrono::milliseconds default_flush{400};

  /// @enum Reason
  /// @brief Why the console is sent to rest.
  ///
  /// @var Reason leaving
  /// @brief The player is leaving and asked for the console to rest.
  ///
  /// @var Reason host_sleep
  /// @brief The host device is going to sleep.
  enum class Reason : std::uint8_t { leaving, host_sleep };
  /// @enum Outcome
  /// @brief What became of the request, for the session's outcome.
  ///
  /// @var Outcome none
  /// @brief No rest was asked for.
  ///
  /// @var Outcome sent
  /// @brief The console accepted the request.
  ///
  /// @var Outcome refused
  /// @brief The request could not be made or sent, or is still waiting.
  enum class Outcome : std::uint8_t { none, sent, refused };

  /// @brief Asks for rest mode and wakes the session thread.
  ///
  /// A second request while one is under way only upgrades its reason to
  /// Reason::host_sleep.
  ///
  /// @param reason Why.
  /// @return `true` when queued or already under way; `false` for a cloud
  ///     session, a session not running, or one already parked for sleep, in
  ///     which case the caller ends the session another way.
  auto request(Reason reason) -> bool;
  /// @brief Reads the outcome.
  ///
  /// @return Outcome::none before any request; Outcome::sent once the console
  ///     accepted it; Outcome::refused otherwise.
  auto outcome() const noexcept -> Outcome;
  /// @brief Reports whether no rest was asked for.
  ///
  /// @return `true` before any request.
  auto idle() const noexcept -> bool;
  /// @brief Reports whether a request waits to be sent.
  ///
  /// @return `true` until the session loop sends it.
  auto queued() const noexcept -> bool;
  /// @brief Reports whether the console accepted the request and the session is
  /// flushing.
  ///
  /// @return `true` after a successful send.
  auto flushing() const noexcept -> bool;
  /// @brief Reports whether the rest is for a host sleep.
  ///
  /// @return `true` when the reason is Reason::host_sleep.
  auto for_sleep() const noexcept -> bool;

 private:
  friend class ConsoleCommands;

  /// @enum Phase
  /// @brief Where the request stands.
  ///
  /// @var Phase idle
  /// @brief Nothing was asked.
  ///
  /// @var Phase queued
  /// @brief The request waits for the session loop to send it.
  ///
  /// @var Phase flushing
  /// @brief The console accepted it; the session runs out the flush time.
  ///
  /// @var Phase::refused
  /// @brief The request could not be made or sending failed.
  enum class Phase : std::uint8_t { idle, queued, flushing, refused };

  /// @brief What one pass of RestRequest::advance found.
  struct Step {
    /// @brief Whether the rest is over and the session should end.
    bool finished{};
    /// @brief How long the session loop may sleep.
    std::chrono::milliseconds wait{};
  };

  /// @brief Builds the request with nothing asked.
  ///
  /// @param threading Provider of the request's mutex; must outlive this
  ///     object.
  /// @param route Where the request goes; must outlive this object.
  /// @param lifecycle The session's lifecycle; must outlive this object.
  /// @param clock The monotonic clock the flush is timed by; must outlive this
  ///     object.
  /// @param flush How long to keep running after sending.
  RestRequest(platform::ThreadingProvider& threading, const CommandRoute& route,
              const LockedSessionLifecycle& lifecycle,
              platform::ClockProvider& clock,
              std::chrono::milliseconds flush);

  /// @brief Sends a queued request and times its flush; called by the session
  /// loop. Blocks on the link while sending.
  ///
  /// @param wait The longest the loop would sleep.
  /// @return Not finished with @p wait when idle; finished once refused or the
  ///     flush time passed; otherwise not finished with the shorter of @p wait
  ///     and the flush time left.
  auto advance(std::chrono::milliseconds wait) -> Step;

  /// @brief Where the request goes.
  const CommandRoute& route_;
  /// @brief The session's lifecycle.
  const LockedSessionLifecycle& lifecycle_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief How long to keep running after sending.
  std::chrono::milliseconds flush_;
  /// @brief Guards the fields below.
  mutable platform::Mutex mutex_;
  /// @brief Where the request stands.
  Phase phase_{Phase::idle};
  /// @brief Why rest was asked for.
  Reason reason_{Reason::leaving};
  /// @brief When the request was sent, in monotonic microseconds; 0 before.
  std::uint64_t flushing_since_us_{};
};

}  // namespace libchiaking::play
