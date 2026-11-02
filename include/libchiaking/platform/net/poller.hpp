/// @file
/// @brief Declares the readiness-waiting port: PollEvent, the request and
/// result types, Poller and PollerProvider.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace libchiaking::platform {

/// @brief Bit flags naming the kinds of readiness a poller can wait for and
/// report.
enum class PollEvent : std::uint32_t {
  /// @brief No readiness.
  none = 0,
  /// @brief Data, a pending connection or an end-of-stream condition can be
  /// read without waiting.
  readable = 1u << 0,
  /// @brief A send would not wait, or a TCP connect has finished and
  /// TcpSocket::complete_connect can be called.
  writable = 1u << 1,
};

/// @brief Combines two sets of PollEvent flags.
///
/// @param left One set of flags.
/// @param right The other set of flags.
/// @return Every flag set in either operand.
constexpr PollEvent operator|(PollEvent left, PollEvent right) noexcept {
  return static_cast<PollEvent>(static_cast<std::uint32_t>(left) |
                                static_cast<std::uint32_t>(right));
}

/// @brief Returns the PollEvent flags present in both operands; use it to test
/// a readiness mask.
///
/// @param left One set of flags.
/// @param right The other set of flags.
/// @return Only the flags set in both operands; PollEvent::none when they share
///     none.
constexpr PollEvent operator&(PollEvent left, PollEvent right) noexcept {
  return static_cast<PollEvent>(static_cast<std::uint32_t>(left) &
                                static_cast<std::uint32_t>(right));
}

/// @brief Asks a Poller to watch one socket for one or more kinds of
/// readiness.
struct PollRequest {
  /// @brief The socket or listener to watch. Borrowed: it must stay alive and
  /// open until the wait returns, and must come from the same NetworkProvider
  /// as the poller.
  const Pollable* pollable{nullptr};
  /// @brief Caller-chosen value copied into the matching PollReadiness so the
  /// caller can tell which request became ready.
  std::uint64_t tag{0};
  /// @brief The readiness to wait for.
  PollEvent interest{PollEvent::none};
};

/// @brief Reports that one watched socket became ready.
struct PollReadiness {
  /// @brief The PollRequest::tag of the request that became ready.
  std::uint64_t tag{0};
  /// @brief The readiness observed. The stock poller reports a socket error
  /// or hang-up as both readable and writable, so that the next socket call
  /// returns the error.
  PollEvent ready{PollEvent::none};
};

/// @brief Outcome of one Poller::wait call.
///
/// When woke and stop_requested are both false and ready is empty, the timeout
/// elapsed.
struct PollWaitResult {
  /// @brief True when Poller::wake ended the wait. Any number of wake
  /// calls made since the previous wait are reported once.
  bool woke{false};
  /// @brief True when the wait ended because the stop token was stopped. woke
  /// is then false and ready is empty.
  bool stop_requested{false};
  /// @brief The requests that became ready, in request order. Requests that
  /// are not ready are absent.
  std::vector<PollReadiness> ready;
};

class PollerProvider;
class NetworkProvider;

/// @brief Waits for readiness on a set of sockets, with a timeout, a wake-up
/// call and cooperative cancellation.
///
/// Implemented by the host frontend, or by the stock poller that
/// DefaultPollerProvider creates. The library blocks a network thread in wait()
/// and calls wake() from other threads when that thread has new work.
///
/// A poller holds no registrations between calls: every wait() names the full
/// set of sockets to watch.
class Poller {
 public:
  /// @brief Releases the poller.
  virtual ~Poller() = default;
  /// @brief Returns the NetworkProvider that created this poller. Only
  /// pollables with the same owner can be waited on.
  ///
  /// @return The creating provider; it outlives the poller.
  virtual const NetworkProvider& owner() const noexcept = 0;
  /// @brief Makes the current wait(), or the next one if none is in progress,
  /// return with PollWaitResult::woke set.
  ///
  /// Safe to call from any thread while another thread is inside wait(). It
  /// does not block. Repeated calls before a wait collapse into a single
  /// wake-up.
  ///
  /// @return Nothing on success.
  /// @retval NetError::system_failure The wake-up could not be delivered.
  [[nodiscard]] virtual auto wake() -> Expected<void, NetError> = 0;
  /// @brief Blocks the calling thread until a socket is ready, wake() is
  /// called, the stop token is stopped or the timeout elapses.
  ///
  /// A stop token that is already stopped makes the call return at once. A
  /// stop followed by wake() must end a wait in progress and be reported as
  /// PollWaitResult::stop_requested, not as woke. The stock poller also
  /// notices a stop without a wake() within about ten milliseconds.
  ///
  /// @param requests The sockets to watch for this call only. May be empty to
  ///     wait for wake(), stop or the timeout alone.
  /// @param timeout Longest time to wait. Zero or less checks readiness once
  ///     and returns without waiting.
  /// @param stop_token Cancels the wait when stopped.
  /// @return How the wait ended and which requests are ready.
  /// @retval NetError::wrong_provider A request has a null pollable or one
  ///     whose owner is not this poller's owner. Nothing is waited on.
  /// @retval NetError::invalid_argument More requests were passed than the
  ///     poller supports; the stock poller accepts 8192.
  /// @retval NetError::system_failure A watched socket is closed or invalid,
  ///     or the system wait failed.
  [[nodiscard]] virtual auto wait(std::span<const PollRequest> requests,
                                  std::chrono::nanoseconds timeout,
                                  const StopToken& stop_token)
      -> Expected<PollWaitResult, NetError> = 0;
};

/// @brief Port that creates pollers.
///
/// Implemented by the host frontend, or by DefaultPollerProvider in this tree.
/// The library reaches it through NetworkProvider::pollers.
class PollerProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~PollerProvider() = default;
  /// @brief Creates an independent poller.
  ///
  /// @return A poller owned by the caller. Its owner() is the NetworkProvider
  ///     this provider belongs to.
  /// @retval NetError::no_buffers The poller's wake-up channel or memory
  ///     could not be allocated.
  /// @retval NetError::system_failure Any other failure.
  [[nodiscard]] virtual auto create_poller()
      -> Expected<std::unique_ptr<Poller>, NetError> = 0;
};

}  // namespace libchiaking::platform
