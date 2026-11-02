/// @file
/// @brief Declares usecases::ReadLibrary, the use case that reads what a user
/// owns: the LegacyCloudStream library from the commerce list and the PS5
/// library from its own scan, held together.
#pragma once

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/usecases/fetch_progress.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

namespace libchiaking::catalog::usecases {

/// @brief Reads and holds a user's library.
///
/// Borrows the HTTP provider, clocks, log and held answers, which must outlive
/// it. Safe to call from several threads at once: all shared state lives in
/// HeldAnswers. Not copyable.
class ReadLibrary {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A held library.
  using LibraryPtr = HeldAnswers::LibraryPtr;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends every request. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where answers are held. Must outlive this object.
  ReadLibrary(platform::HttpProvider& http,
              const platform::ClockProvider& clock,
              const platform::ClockProvider& wall_clock,
              platform::DiagnosticsProvider* log, HeldAnswers& held) noexcept;
  /// @brief Not copyable: it borrows shared state.
  ReadLibrary(const ReadLibrary&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const ReadLibrary&) -> ReadLibrary& = delete;

  /// @brief The user's library, LegacyCloudStream and PS5 parts together.
  ///
  /// A held library whose PS5 part was fully read is served; otherwise the
  /// commerce list (or the held LegacyCloudStream part) is completed with a
  /// fresh PS5 scan and held. When the commerce list fails the PS5 scan is
  /// still read and held alone.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param progress Told the activities and pages of a read this caller
  ///     makes.
  /// @return The library; the sign-in failure; the commerce list's failure.
  [[nodiscard]] auto library(users::PsUser& user, const Stop& stop,
                             const FetchProgress& progress = {})
      -> Expected<LibraryPtr, Unanswered>;
  /// @brief Rereads the user's whole library, ignoring what is held.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return As library.
  [[nodiscard]] auto afresh(users::PsUser& user, const Stop& stop)
      -> Expected<LibraryPtr, Unanswered>;
  /// @brief What the user owns, fetching the library when needed.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param progress Told the read's activities and pages.
  /// @return The library, or the rechecked-only titles marked unavailable when
  ///     it could not be read; the failure when it ends the load.
  [[nodiscard]] auto owned_by(users::PsUser& user, const Stop& stop,
                              const FetchProgress& progress)
      -> Expected<HeldAnswers::Owned, Unanswered>;
  /// @brief A held library whose PS5 part was fully read.
  ///
  /// @param account The account.
  /// @return The library, or null when none is held or its PS5 part is partial
  ///     or unknown.
  auto whole_fresh(AccountId account) -> LibraryPtr;

 private:
  /// @brief A library with its PS5 part and the scan it came from.
  struct Whole {
    /// @brief The library.
    Library library;
    /// @brief The PS5 scan; unread when the scan failed.
    OwnedPs5Games ps5;
  };

  /// @brief Reads the LegacyCloudStream library from the commerce list.
  ///
  /// PS5 records are dropped; concepts of streamed PS4 copies are read
  /// separately when any lacks one, and a failure there is only logged.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param progress Told the activities and pages.
  /// @return The library; the commerce list's failure.
  [[nodiscard]] auto legacy_cloud_stream_part(users::PsUser& user,
                                              net::http::Client& client,
                                              const FetchProgress& progress)
      -> Expected<Library, Unanswered>;
  /// @brief Completes a LegacyCloudStream library with a fresh PS5 scan and
  /// holds both.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param legacy_cloud_stream The LegacyCloudStream library, or why it could
  ///     not be read; in that case the PS5 scan is held alone unless the
  ///     failure ends the load.
  /// @param progress Told the PS5 scan's activity and pages.
  /// @return The held library; the LegacyCloudStream failure; a cancellation.
  [[nodiscard]] auto kept_whole(
      users::PsUser& user, net::http::Client& client,
      Expected<Library, Unanswered> legacy_cloud_stream,
      const FetchProgress& progress) -> Expected<LibraryPtr, Unanswered>;
  /// @brief Scans and holds the PS5 library while the commerce list is
  /// unreadable; a failed scan is only logged.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param pages Counts the pages read.
  void scan_ps5_alone(users::PsUser& user, net::http::Client& client,
                      PageCount pages);
  /// @brief Adds a fresh PS5 scan to a LegacyCloudStream library.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param legacy_cloud_stream The LegacyCloudStream library.
  /// @param pages Counts the pages read.
  /// @return The whole library, with PS5 ownership unavailable when the scan
  ///     failed; a cancellation.
  [[nodiscard]] auto with_ps5_part(users::PsUser& user,
                                   net::http::Client& client,
                                   Library legacy_cloud_stream, PageCount pages)
      -> Expected<Whole, Unanswered>;

  /// @brief Sends every request; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
};

}  // namespace libchiaking::catalog::usecases
