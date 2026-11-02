/// @file
/// @brief Declares the stop token a caller passes to cancel a call, its
/// listener and state, and the stop sources that request a stop.
#pragma once

#include <memory>
#include <utility>

namespace libchiaking::platform {

/// @brief Told when a stop is requested, so a waiter can wake at once instead
/// of polling StopState::stop_requested.
///
/// Implemented by waiters such as a provider's condition variable and
/// registered with StopState::listen.
class StopListener {
 public:
  /// @brief Destroys the listener.
  ///
  /// @pre It is no longer registered with any stop state.
  virtual ~StopListener() = default;
  /// @brief Called once on the thread that requests the stop, after
  /// StopState::stop_requested has turned true.
  ///
  /// It must not block, and must not register or unregister listeners.
  virtual void on_stop_requested() noexcept = 0;
};

/// @brief Shared flag behind a StopToken.
///
/// Implemented by whoever creates stop tokens: a ThreadingProvider, StopSource,
/// or library code that combines a caller's token with a deadline. Tokens read
/// it from any thread, so both queries must be safe to call concurrently.
class StopState {
 public:
  /// @brief Destroys the state once the last source or token referring to it
  /// is gone.
  virtual ~StopState() = default;
  /// @brief Whether work watching this state should stop.
  ///
  /// @return False until a stop is requested, true ever after.
  virtual bool stop_requested() const noexcept = 0;
  /// @brief Whether the stop was caused by a deadline running out and not by
  /// a caller cancelling.
  ///
  /// The library uses it to report a timeout instead of a cancellation.
  ///
  /// @return False by default; overridden by states that carry a deadline.
  virtual bool deadline_expired() const noexcept { return false; }
  /// @brief Registers a listener to be told when a stop is requested.
  ///
  /// Only the stop requested after registration is reported; a caller checks
  /// stop_requested() once registered to catch one that came before.
  ///
  /// @param listener The listener; stays registered until ignore() returns.
  /// @return True when the listener will be told; false by default, for a
  ///     state that can only be polled.
  virtual bool listen(StopListener& listener) const noexcept {
    static_cast<void>(listener);
    return false;
  }
  /// @brief Unregisters a listener registered with listen().
  ///
  /// Once it returns the listener is not being called and will not be.
  ///
  /// @param listener The listener; ignored when not registered.
  virtual void ignore(StopListener& listener) const noexcept {
    static_cast<void>(listener);
  }
};

/// @brief Read-only view of a stop request, passed to work that can be
/// cancelled.
///
/// Cheap to copy. A copy shares the state and keeps it alive, so a token may
/// outlive the source it came from. A default-constructed token is attached to
/// nothing and never requests a stop.
class StopToken {
 public:
  /// @brief Constructs a token attached to no state; it never requests a
  /// stop.
  StopToken() = default;
  /// @brief Whether the holder should abandon its work.
  ///
  /// @return True once the attached state requests a stop; always false for a
  ///     token attached to nothing.
  bool stop_requested() const noexcept {
    return state_ != nullptr && state_->stop_requested();
  }
  /// @brief Whether the stop came from a deadline running out.
  ///
  /// @return True when the attached state reports an expired deadline; a
  ///     caller then reports a timeout and not a cancellation. Always false
  ///     for a token attached to nothing.
  bool deadline_expired() const noexcept {
    return state_ != nullptr && state_->deadline_expired();
  }
  /// @brief Whether the token is attached to a state at all.
  ///
  /// @return False for a default-constructed token. It says nothing about
  ///     whether a stop was requested.
  explicit operator bool() const noexcept { return state_ != nullptr; }
  /// @brief Registers a listener with the attached state, as
  /// StopState::listen describes.
  ///
  /// @param listener The listener; stays registered until ignore() returns.
  /// @return True when the listener will be told of the stop; false for a
  ///     token attached to nothing or to a state that can only be polled.
  bool listen(StopListener& listener) const noexcept {
    return state_ != nullptr && state_->listen(listener);
  }
  /// @brief Unregisters a listener registered with listen().
  ///
  /// @param listener The listener.
  void ignore(StopListener& listener) const noexcept {
    if (state_ != nullptr) state_->ignore(listener);
  }

  /// @brief Attaches a token to a stop state.
  ///
  /// @param state State to watch; shared with the source. A null pointer
  ///     gives a token that never requests a stop.
  explicit StopToken(std::shared_ptr<const StopState> state) noexcept
      : state_(std::move(state)) {}

 private:
  std::shared_ptr<const StopState> state_;
};

namespace native {

/// @brief The writing end of a stop request, created by a ThreadingProvider.
///
/// Implemented by the provider. The owner calls request_stop() to cancel every
/// piece of work holding one of its tokens. A provider's stop-aware condition
/// variable waits must return ConditionWaitStatus::stop_requested once the
/// source is triggered, even if nobody notifies the condition variable.
class StopSource {
 public:
  /// @brief Destroys the source. Tokens already handed out stay valid.
  virtual ~StopSource() = default;
  /// @brief A token that observes this source.
  ///
  /// @return A token sharing this source's state. Every call returns an
  ///     equivalent token.
  virtual StopToken token() const noexcept = 0;
  /// @brief Requests a stop. Safe to call from any thread and more than once.
  ///
  /// @return True only for the call that first requested the stop; false if
  ///     one had already been requested.
  virtual bool request_stop() noexcept = 0;
};

}  // namespace native

/// @brief A stop source that needs no ThreadingProvider.
///
/// It is a shared flag for code that wants StopToken cancellation
/// without a provider at hand. It never reports an expired deadline. Copies
/// share the same flag.
class StopSource {
 public:
  /// @brief Creates a source with no stop requested.
  StopSource();

  /// @brief A token that observes this source and keeps its flag alive.
  ///
  /// The token's state tells listeners registered through
  /// StopToken::listen when the stop is requested.
  ///
  /// @return A token sharing this source's flag. Safe to call from any
  ///     thread.
  auto token() const noexcept -> StopToken;
  /// @brief Requests a stop and tells every registered listener. Safe to call
  /// from any thread and more than once.
  ///
  /// @return True only for the call that first requested the stop.
  auto request_stop() noexcept -> bool;
  /// @brief Whether a stop has been requested through this source or a copy
  /// of it.
  ///
  /// @return False until request_stop() has been called on this source or a
  ///     copy, true ever after.
  auto stop_requested() const noexcept -> bool;

 private:
  /// @brief The flag copies of a source share, with the listeners told when
  /// it is raised; defined by the stock threading provider.
  class Flag;

  /// @brief The shared flag.
  std::shared_ptr<Flag> flag_;
};

}  // namespace libchiaking::platform
