/// @file
/// @brief Declares KnownConsoles::Core, the inside of the console registry:
/// the roster, the shared network search, the console library and the find,
/// pair and wake use cases, wired to the host's providers.
///
/// KnownConsoles is the frontend's face of the consoles context; each of its
/// calls is forwarded to one of the parts held here, and every Console
/// reaches its registry's parts through home().
#pragma once

#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/roster/roster.hpp"
#include "src/core/consoles/adapters/titles/title_start.hpp"
#include "src/core/consoles/adapters/search/shared_search.hpp"
#include "src/core/consoles/adapters/titles/console_library.hpp"
#include "src/core/consoles/usecases/find.hpp"
#include "src/core/consoles/usecases/pair.hpp"
#include "src/core/consoles/usecases/wake.hpp"

#include <atomic>

namespace libchiaking::consoles {

/// @brief The console registry's parts, made together and living as long as
/// the KnownConsoles that owns them.
///
/// Owned by its KnownConsoles through a unique pointer. The providers it was
/// made with must outlive it. The accessors may be called from any thread;
/// the parts they return do their own locking.
class KnownConsoles::Core {
 public:
  /// @brief Wires the registry's parts to the host's providers.
  ///
  /// @param face The KnownConsoles that owns this core; the console library
  ///     reaches the registry through it.
  /// @param network Sockets for searching, pairing and waking.
  /// @param http HTTP for PSN requests.
  /// @param crypto Ciphers and hashes for pairing.
  /// @param random Secure randomness for pairing.
  /// @param store Where consoles and pairings are saved.
  /// @param threading Makes the registry's locks, condition variables and
  ///     sleeps.
  /// @param clock The monotonic clock for deadlines and pauses.
  /// @param internet How a console is paired over the internet; null when the
  ///     host offers no internet pairing.
  /// @param log Where the registry logs; null logs nothing.
  Core(KnownConsoles& face, platform::NetworkProvider& network,
       platform::HttpProvider& http, platform::CryptoProvider& crypto,
       platform::SecureRandomProvider& random, ConsoleStore& store,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, InternetPairing* internet,
       platform::DiagnosticsProvider* log);
  /// @brief Not copyable: the parts refer to each other.
  Core(const Core&) = delete;
  /// @brief Not copyable.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief The list of known consoles.
  ///
  /// @return The roster.
  auto roster() noexcept -> Roster& { return roster_; }
  /// @brief Reads what is installed on consoles and what the account played
  /// recently.
  ///
  /// @return The console library.
  auto library() noexcept -> ConsoleLibrary& { return library_; }
  /// @brief Finds consoles on the local network and checks known ones.
  ///
  /// @return The find use case.
  auto find() noexcept -> usecases::Find& { return find_; }
  /// @brief Pairs consoles at home by PIN or over the internet from the
  /// account.
  ///
  /// @return The pair use case.
  auto pair() noexcept -> usecases::Pair& { return pair_; }
  /// @brief Wakes consoles from rest mode.
  ///
  /// @return The wake use case.
  auto wake() noexcept -> usecases::Wake& { return wake_; }
  /// @brief The HTTP provider the registry was made with.
  ///
  /// @return The provider.
  auto http() const noexcept -> platform::HttpProvider& { return http_; }
  /// @brief The threading provider the registry's locks and sleeps are made
  /// with.
  ///
  /// @return The provider.
  auto threading() const noexcept -> platform::ThreadingProvider& {
    return threading_;
  }
  /// @brief The monotonic clock the registry measures time by.
  ///
  /// @return The clock.
  auto clock() const noexcept -> const platform::ClockProvider& {
    return clock_;
  }
  /// @brief Pauses the waits of invitation joins and alert watches.
  ///
  /// @return The pause, shared by every thread that waits through it.
  auto pause() noexcept -> net::http::Waiter& { return pause_; }
  /// @brief Where the registry logs.
  ///
  /// @return The provider, or null when nothing is logged.
  auto log() const noexcept -> platform::DiagnosticsProvider* { return log_; }
  /// @brief Where a console sends a session it is asked to start.
  ///
  /// @return The play context's starting port, or null when none is
  ///     connected.
  auto starting() const noexcept -> Starting* {
    return starting_.load(std::memory_order_acquire);
  }
  /// @brief Connects or disconnects the play context's starting port.
  ///
  /// ChiakiNg connects it once the play context exists and disconnects it
  /// before that context goes away. Safe to call from any thread while
  /// consoles read it.
  ///
  /// @param starting The port, or null to disconnect it; must outlive its
  ///     connection.
  void start_through(Starting* starting) noexcept {
    starting_.store(starting, std::memory_order_release);
  }
  /// @brief The WebSocket provider game invitations are watched through.
  ///
  /// @return The provider, or null when none is connected.
  auto websocket() const noexcept -> platform::WebSocketProvider* {
    return websocket_.load(std::memory_order_acquire);
  }
  /// @brief Connects or disconnects the WebSocket provider.
  ///
  /// @param websocket The provider, or null to disconnect it; must outlive
  ///     its connection.
  void watch_through(platform::WebSocketProvider* websocket) noexcept {
    websocket_.store(websocket, std::memory_order_release);
  }

 private:
  /// @brief The HTTP provider for PSN requests.
  platform::HttpProvider& http_;
  /// @brief Makes the registry's locks, condition variables and sleeps.
  platform::ThreadingProvider& threading_;
  /// @brief The clock the registry measures time by.
  const platform::ClockProvider& clock_;
  /// @brief Where the registry logs; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
  /// @brief Pauses the waits of invitation joins and alert watches.
  TitleStart::Pause pause_;
  /// @brief The known consoles.
  Roster roster_;
  /// @brief The one network search that concurrent finds and checks share.
  SharedSearch search_;
  /// @brief Installed titles and recently played games.
  ConsoleLibrary library_;
  /// @brief The find use case.
  usecases::Find find_;
  /// @brief The pair use case.
  usecases::Pair pair_;
  /// @brief The wake use case.
  usecases::Wake wake_;
  /// @brief The play context's starting port; null until connected.
  std::atomic<Starting*> starting_{};
  /// @brief The WebSocket provider game invitations are watched through;
  /// null until connected.
  std::atomic<platform::WebSocketProvider*> websocket_{};
};

}  // namespace libchiaking::consoles
