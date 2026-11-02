/// @file
/// @brief Declares TrustedClock, the source of time used to judge release
/// dates.
#pragma once

#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::catalog {

/// @brief A source of the current time that the user cannot change, implemented
/// by the embedder.
///
/// The Catalog asks it only to decide whether a game has been released. It is
/// called on whichever thread calls Catalog::can_launch or
/// Catalog::clear_for_launch, or loads games that include a title with a
/// release date, and must outlive the catalog it was given to.
///
/// @see ReleaseDate::state
class TrustedClock {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;
  /// @brief Told the host name of each time server as a sync turns to it,
  /// such as `time.cloudflare.com`.
  using ServerObserver = std::function<void(std::string_view host)>;

  /// @brief Where the trusted time currently held came from, for a
  /// diagnostics screen.
  ///
  /// Plain data copied out of the clock; it does not change when the clock
  /// syncs again.
  struct Sync {
    /// @brief Short name of the time provider that answered, such as
    /// `cloudflare` or `google` for the library's own NTP clock.
    std::string provider;
    /// @brief The trusted wall-clock time at the moment of the sync, in
    /// milliseconds since the Unix epoch; not device time.
    Time synced_at{};

    /// @brief Compares the provider and the sync time.
    friend auto operator==(const Sync&, const Sync&) -> bool = default;
  };

  /// @brief Allows an implementation to be destroyed through this interface.
  virtual ~TrustedClock() = default;

  /// @brief Reports the current trusted time.
  ///
  /// @return The time, or nothing when no trusted time is available yet; a
  ///     game with a release date then cannot be cleared for launch.
  [[nodiscard]] virtual auto now() const noexcept -> std::optional<Time> = 0;

  /// @brief Reports the current trusted time, first obtaining it when none is
  /// held.
  ///
  /// The Catalog calls it only when a title has a release date: while
  /// loading games that include such a title, and when the release check of
  /// Catalog::clear_for_launch runs. Unlike now, an implementation may block
  /// here on the network; the library's NTP clock returns a held time without
  /// a request, otherwise queries its providers in turn, telling each host it
  /// turns to, and after a failed sync waits 30 seconds before trying again.
  /// The default implementation makes no request, tells nothing and returns
  /// now.
  ///
  /// @param stop Cancels any request; a cancelled sync is not counted as a
  ///     failure.
  /// @param asking Told the host of each server as the sync turns to it,
  ///     never the same one twice in a row, on the calling thread; the
  ///     Catalog shows it as FetchState::time_server while loading games.
  ///     May be empty.
  /// @return The time, or nothing when no trusted time could be obtained,
  ///     the sync was cancelled, or a recent failure holds off a new try; a
  ///     game with a release date then cannot be cleared for launch.
  [[nodiscard]] virtual auto refresh(const platform::StopToken& stop,
                                     const ServerObserver& asking)
      -> std::optional<Time> {
    static_cast<void>(stop);
    static_cast<void>(asking);
    return now();
  }

  /// @brief Describes the sync behind the trusted time currently held.
  ///
  /// Makes no request. Read by Catalog::how_fetched for its FetchReport.
  ///
  /// @return The provider and time of the sync, or nothing when no trusted
  ///     time is held or the held one has expired. The default
  ///     implementation always returns nothing.
  [[nodiscard]] virtual auto last_sync() const -> std::optional<Sync> {
    return std::nullopt;
  }
};

}  // namespace libchiaking::catalog
