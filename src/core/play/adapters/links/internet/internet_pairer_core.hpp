/// @file
/// @brief Declares InternetPairer::Core, which pairs with a console over the
/// internet by running a pairing-only internet link until the console
/// registers the client.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/links/link.hpp"

#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace libchiaking::play {

/// @brief The implementation behind the public InternetPairer: one pairing
/// at a time, refused while a session is playing.
///
/// A pairing reaches the console through PSN like an internet start, but
/// stops as soon as the console sends its registration. The pairing flag is
/// atomic and the session watch is under a mutex, so any thread may ask
/// whether a pairing runs. The platform must outlive it. Not copyable.
class InternetPairer::Core {
 public:
  /// @brief How far an internet pairing has got.
  using Stage = consoles::InternetPairingStage;

  /// @brief One pairing run: drives a pairing-only link on the calling thread
  /// until the console registers, fails or the run is stopped.
  ///
  /// Another thread may cancel it. Not copyable.
  class Attempt {
   public:
    /// @brief How long each wait on the link lasts between pumps.
    static constexpr std::chrono::milliseconds wait_step{100};

    /// @brief Whom to pair as, and with which console. Plain data.
    struct Request {
      /// @brief The user to pair as.
      std::shared_ptr<users::PsUser> user;
      /// @brief The console's PSN device id.
      std::array<std::byte, psn_device_uid_size> device_uid{};
      /// @brief The firmware target to register with.
      net::protocol::remote_play::RegistrationTarget target{
          net::protocol::remote_play::RegistrationTarget::ps5_1};
    };

    /// @brief Why a pairing run failed. Plain data.
    struct Failure {
      /// @brief Whether the run was stopped.
      bool cancelled{};
      /// @brief The link's error.
      net::protocol::remote_play::StreamError error{
          net::protocol::remote_play::StreamError::invalid_state};
      /// @brief How far the pairing got.
      Stage stage{Stage::starting};
      /// @brief The link's failure, when it recorded one.
      std::optional<LinkFailure> detail;

      /// @brief Turns the failure into the public pairing failure.
      ///
      /// @return Cancelled; malformed or not available for a configuration
      ///     failure; refused, timed out or unreadable reply for a registration
      ///     failure; unanswered with missing Remote Play access for a refused
      ///     token or HTTP 401 or 403, or rate limited for HTTP 429; otherwise
      ///     unreachable. The stage, HTTP status and PS reason are carried.
      auto typed() const -> consoles::PairingFailure;
    };

    /// @brief Builds a run.
    ///
    /// @param platform The host's providers; must outlive the run.
    /// @param log Receives log lines; must outlive the run.
    /// @param watcher Told the stages and progress; may be null.
    /// @param caller The caller's stop.
    Attempt(platform::PlatformFactory& platform,
            platform::DiagnosticsProvider& log,
            consoles::PairingWatcher* watcher = nullptr,
            platform::StopToken caller = {});
    /// @brief Not copyable; a run is used once.
    Attempt(const Attempt&) = delete;
    /// @brief Not copy-assignable; a run is used once.
    auto operator=(const Attempt&) -> Attempt& = delete;
    /// @brief Releases the run.
    ~Attempt();

    /// @brief Builds a pairing-only link for the request and runs it.
    ///
    /// Blocks for up to the default internet start timeout.
    ///
    /// @param request Whom to pair as, and with which console.
    /// @return The registered console; a failure for a bad target or
    ///     configuration, a stop, or the link's failure.
    [[nodiscard]] auto run(Request request)
        -> Expected<net::protocol::remote_play::RegisteredConsole, Failure>;
    /// @brief Runs a pairing-only link until the console registers; blocks on
    /// the link. Call once.
    ///
    /// @param session The link.
    /// @return The registered console; a failure with Error::invalid_argument
    ///     for a missing link, a cancellation when already run or stopped, or
    ///     the link's failure.
    [[nodiscard]] auto run(std::unique_ptr<Link> session)
        -> Expected<net::protocol::remote_play::RegisteredConsole, Failure>;
    /// @brief Stops the run from any thread.
    void cancel() noexcept;
    /// @brief Reads how far the pairing got.
    ///
    /// @return The stage.
    auto stage() const noexcept -> Stage {
      return stage_.load(std::memory_order_acquire);
    }

   private:
    /// @brief Passes the link's progress on as pairing stages and steps, and
    /// keeps the registration; defined in the implementation.
    class Events;
    /// @brief Stops when the run or the caller stops; defined in the
    /// implementation.
    class Stop;

    /// @brief Reports whether the run was stopped.
    ///
    /// @return `true` once cancelled or once the caller stopped.
    auto stopped() const noexcept -> bool;

    /// @brief The host's providers.
    platform::PlatformFactory& platform_;
    /// @brief Receives log lines.
    platform::DiagnosticsProvider& log_;
    /// @brief Told the stages and progress; may be null.
    consoles::PairingWatcher* watcher_;
    /// @brief The caller's stop.
    platform::StopToken caller_;
    /// @brief The run's own stop.
    platform::StopSource stop_;
    /// @brief Guards the driver pointer against a concurrent cancel.
    platform::Mutex mutex_;
    /// @brief Whether the run started.
    std::atomic<bool> started_{false};
    /// @brief Whether the run was cancelled.
    std::atomic<bool> cancelled_{false};
    /// @brief How far the pairing got.
    std::atomic<Stage> stage_{Stage::starting};
    /// @brief The running driver; null outside the run.
    LinkDriver* driver_{};
  };

  /// @brief Builds the pairer.
  ///
  /// @param platform The host's providers; must outlive the pairer.
  /// @param log Receives log lines; null falls back to the platform's, else to
  ///     nothing.
  Core(platform::PlatformFactory& platform, platform::DiagnosticsProvider* log);
  /// @brief Not copyable; it is one per Play.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; it is one per Play.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Pairs with a console over the internet; blocks until it registers,
  /// fails or @p stop fires.
  ///
  /// @param as The user to pair as.
  /// @param which The console's PSN device id.
  /// @param generation The console's generation; its newest firmware target is
  ///     registered with.
  /// @param watcher Told the stages and progress; may be null.
  /// @param stop Cancels the pairing.
  /// @return The paired console; unanswered and signed out without a PSN
  ///     account; busy while another pairing runs; session playing while a
  ///     session is live; malformed for a registration that cannot be read; or
  ///     the run's failure.
  [[nodiscard]] auto pair(users::PsUser& as, const consoles::DeviceUid& which,
                          consoles::Generation generation,
                          consoles::PairingWatcher* watcher,
                          const platform::StopToken& stop)
      -> Expected<consoles::PairedConsole, consoles::PairingFailure>;
  /// @brief Reports whether a pairing runs.
  ///
  /// @return `true` while one runs.
  auto is_pairing() const noexcept -> bool {
    return pairing_.load(std::memory_order_acquire);
  }
  /// @brief Sets how the pairer asks whether a session is playing.
  ///
  /// @param playing The question; empty to stop asking.
  void watch_sessions(std::function<bool()> playing);

 private:
  /// @brief Holds the pairing flag for a scope when it was free.
  class Turn {
   public:
    /// @brief Takes the flag when it is free.
    ///
    /// @param pairing The flag; must outlive the turn.
    explicit Turn(std::atomic<bool>& pairing) noexcept;
    /// @brief Not copyable; a turn is scoped.
    Turn(const Turn&) = delete;
    /// @brief Not copy-assignable; a turn is scoped.
    auto operator=(const Turn&) -> Turn& = delete;
    /// @brief Releases the flag when this turn took it.
    ~Turn();

    /// @brief Reports whether this turn took the flag.
    ///
    /// @return `true` when no other pairing ran.
    auto taken() const noexcept -> bool { return taken_; }

   private:
    /// @brief The flag.
    std::atomic<bool>& pairing_;
    /// @brief Whether this turn took it.
    bool taken_;
  };

  /// @brief The host's providers.
  platform::PlatformFactory& platform_;
  /// @brief Receives log lines.
  platform::DiagnosticsProvider& log_;
  /// @brief Whether a pairing runs.
  std::atomic<bool> pairing_{false};
  /// @brief Guards the session question.
  platform::Mutex sessions_mutex_;
  /// @brief Asks whether a session is playing; empty when unset.
  std::function<bool()> session_playing_;
};

}  // namespace libchiaking::play
