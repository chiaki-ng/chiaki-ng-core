/// @file
/// @brief Declares OwnedPs5Games, the account's PS5 entitlements as read from
/// the mobile internal entitlement list.
///
/// PS5 ownership has its own source: the internal entitlement scan made with
/// the app token. The catalog keeps the raw records beside the Library
/// so that Premium bundles can be judged by exact entitlement id and so that a
/// launch can re-read one record.
#pragma once

#include "src/core/catalog/model/library/ps5_entitlement.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>

#include <chrono>
#include <cstddef>
#include <set>
#include <vector>

namespace libchiaking::catalog {

/// @brief Every PS5 entitlement record one scan returned, with how complete
/// that scan was.
///
/// The records include ones that are not streamable games (trials, add-ons,
/// inactive copies); games narrows them to the ones that are. A value is
/// immutable apart from assignment; with returns a changed copy. The catalog
/// holds it behind a shared pointer to const, so one value is read from
/// several threads at once and must not be modified after it is shared.
class OwnedPs5Games {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief How the scan that produced the records went.
  struct Scan {
    /// @brief How many entitlement records PS returned across every
    /// page, before any were dropped as unparseable or not a game.
    std::size_t records_read{};
    /// @brief How many times the scan started again from the first page
    /// because PS's revisionId changed mid-scan; 0 for a settled scan.
    int revision_restarts{};

    /// @brief Compares both counts.
    friend auto operator==(const Scan&, const Scan&) -> bool = default;
  };

  /// @brief Keeps the records of one scan.
  ///
  /// @param records The parsed entitlement records, in the order PS sent
  ///     them; may be empty.
  /// @param state LibraryState::fetched for a complete scan,
  ///     LibraryState::partial when the revision kept changing or the page
  ///     cap was reached, LibraryState::unavailable when nothing was read.
  /// @param scan The counts the scan reported.
  /// @param fetched_at When the scan finished.
  OwnedPs5Games(std::vector<Ps5Entitlement> records, LibraryState state,
                Scan scan, Time fetched_at) noexcept;

  /// @brief An empty answer meaning PS5 ownership is unknown.
  ///
  /// @param at The time to stamp it with, usually now.
  /// @return No records, LibraryState::unavailable and zero counts.
  static auto unread(Time at) -> OwnedPs5Games;

  /// @brief Every record the scan kept, games or not.
  ///
  /// @return The records, valid while this value lives; possibly empty.
  auto records() const noexcept -> const std::vector<Ps5Entitlement>& {
    return records_;
  }
  /// @brief How complete the scan was.
  ///
  /// @return LibraryState::fetched, LibraryState::partial or
  ///     LibraryState::unavailable.
  auto state() const noexcept -> LibraryState { return state_; }
  /// @brief The counts the scan reported.
  ///
  /// @return The scan counts, valid while this value lives.
  auto scan() const noexcept -> const Scan& { return scan_; }
  /// @brief When the scan finished, or when with last replaced a record.
  ///
  /// @return Wall-clock time in milliseconds.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief The records that are streamable full games, as owned titles.
  ///
  /// A record is dropped when Ps5Entitlement::why_not_a_game names a
  /// reason; a second record with an OwnershipId already seen is dropped.
  ///
  /// @return The owned games in record order; empty when none qualify.
  auto games() const -> std::vector<OwnedTitle>;
  /// @brief Looks up one record by its entitlement id.
  ///
  /// @param id The entitlement id, compared exactly.
  /// @return The record, valid while this value lives, or nullptr when the
  ///     scan held no record with that id.
  auto find(const OwnershipId& id) const noexcept -> const Ps5Entitlement*;
  /// @brief The ids of every record PS marked active, games or not.
  ///
  /// This is the set Premium bundles are judged against: a bundle is owned when
  /// one SKU has every entitlement in it.
  ///
  /// @return The ids whose active flag is exactly true; empty when none.
  auto owned_and_active() const -> std::set<OwnershipId>;
  /// @brief A copy with one record added or replaced, as after a launch
  /// re-read it.
  ///
  /// @param record The fresh record; it replaces the record with the same
  ///     id, or is appended when there is none.
  /// @param checked_at When the record was read; becomes fetched_at.
  /// @return The changed copy. The state and scan counts are kept.
  auto with(Ps5Entitlement record, Time checked_at) const -> OwnedPs5Games;

  /// @brief Compares records, state, counts and time.
  friend auto operator==(const OwnedPs5Games&, const OwnedPs5Games&)
      -> bool = default;

 private:
  /// @brief The parsed records in PS's order.
  std::vector<Ps5Entitlement> records_;
  /// @brief How complete the scan was.
  LibraryState state_;
  /// @brief The counts the scan reported.
  Scan scan_;
  /// @brief When the scan finished or a record was last replaced;
  /// wall-clock milliseconds.
  Time fetched_at_;
};

}  // namespace libchiaking::catalog
