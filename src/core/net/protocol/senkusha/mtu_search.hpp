/// @file
/// @brief Declares ConservativeMtuSearch, the binary search Senkusha uses to
/// find the largest packet a path carries, in either direction.
///
/// The search only decides which size to try next; Client sends the
/// attempts (an MTU request for inbound, a sized audio probe for outbound)
/// and reports each one's fate back.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::senkusha {

/// @brief One try of one MTU candidate.
struct MtuSearchAttempt {
  /// @brief Unique id of the attempt, increasing by one per attempt; the
  /// inbound search sends it as the request id and expects it back as the
  /// video frame index, so it must fit 16 bits there.
  std::uint64_t id{};
  /// @brief The size being tried, in bytes of IP packet.
  std::uint32_t candidate{};
  /// @brief Which try of this candidate it is, from 1 to the attempts
  /// allowed per candidate.
  std::uint32_t attempt_number{};
};

/// @brief Where a search stands.
struct MtuSearchSnapshot {
  /// @brief Largest size known to work, in bytes.
  std::uint32_t lower{};
  /// @brief Smallest size known not to work, or the upper bound, in bytes.
  std::uint32_t upper{};
  /// @brief The size being tried, in bytes.
  std::uint32_t candidate{};
  /// @brief Attempts made in all.
  std::uint32_t attempts_used{};
  /// @brief The answer, which is `lower`, once `upper - lower` is at most 1;
  /// empty until then.
  std::optional<std::uint32_t> result{};
};

/// @brief A binary search for the path MTU that never claims more than it
/// saw work.
///
/// The lower bound is assumed to work. A candidate that succeeds once becomes
/// the new lower bound; one that fails `attempts_per_candidate` times in a
/// row becomes the new upper bound; the next candidate is the midpoint. The
/// search ends when the bounds are 1 apart (or meet, when the first
/// candidate is the upper bound and succeeds), answering the lower bound.
/// Only one attempt may be outstanding at a time.
///
/// Valid from create(); a plain value used from one thread.
class ConservativeMtuSearch {
 public:
  /// @brief Makes a search.
  ///
  /// @param lower Size assumed to work, in bytes.
  /// @param upper Largest size to consider, in bytes.
  /// @param attempts_per_candidate Consecutive failures that condemn a
  ///     candidate; at least 1.
  /// @param initial_candidate First size to try, within the bounds; the
  ///     upper bound for the inbound search, the measured inbound MTU for
  ///     the outbound one.
  /// @param initial_attempt_id Id of the first attempt; not 0.
  /// @return The search.
  /// @retval Error::invalid_configuration The bounds are not
  ///     ascending, no attempts are allowed, the first candidate is outside
  ///     the bounds, or the first id is 0.
  [[nodiscard]] static auto create(std::uint32_t lower, std::uint32_t upper,
                                   std::uint32_t attempts_per_candidate,
                                   std::uint32_t initial_candidate,
                                   std::uint64_t initial_attempt_id = 1)
      -> Expected<ConservativeMtuSearch, Error>;

  /// @brief Opens the next attempt at the current candidate.
  ///
  /// @return The attempt to send.
  /// @retval Error::invalid_state The search is complete or an
  ///     attempt is still outstanding.
  /// @retval Error::arithmetic_overflow The id or a counter would
  ///     wrap.
  [[nodiscard]] auto next_attempt() -> Expected<MtuSearchAttempt, Error>;
  /// @brief Records that the outstanding attempt got through, raising the
  /// lower bound to its candidate and choosing the next one.
  ///
  /// @param id The attempt's id.
  /// @return Nothing.
  /// @retval Error::invalid_state `id` is not the outstanding
  ///     attempt.
  [[nodiscard]] auto record_success(std::uint64_t id) -> Expected<void, Error>;
  /// @brief Records that the outstanding attempt was lost; after enough
  /// losses in a row the upper bound drops to the candidate.
  ///
  /// @param id The attempt's id.
  /// @return Nothing.
  /// @retval Error::invalid_state `id` is not the outstanding
  ///     attempt.
  [[nodiscard]] auto record_failure(std::uint64_t id) -> Expected<void, Error>;
  /// @brief Reads where the search stands.
  ///
  /// @return The bounds, candidate, attempts used, and the result once
  ///     complete.
  MtuSearchSnapshot snapshot() const noexcept;

 private:
  /// @brief Makes an empty search for create() to fill in.
  ConservativeMtuSearch() = default;
  /// @brief Moves the candidate to the midpoint of the bounds, rounded
  /// down, unless the search is complete.
  void select_midpoint() noexcept;
  /// @brief Tells whether the bounds have closed.
  ///
  /// @return Whether `upper - lower` is at most 1.
  bool complete() const noexcept;
  /// @brief Tells whether an id names the outstanding attempt.
  ///
  /// @param id The id to check.
  /// @return Whether an attempt is outstanding and has that id.
  bool matches_outstanding(std::uint64_t id) const noexcept;

  /// @brief Largest size known to work, in bytes.
  std::uint32_t lower_{};
  /// @brief Smallest size known not to work, or the upper bound, in bytes.
  std::uint32_t upper_{};
  /// @brief The size being tried, in bytes.
  std::uint32_t candidate_{};
  /// @brief Consecutive failures that condemn a candidate.
  std::uint32_t attempts_per_candidate_{};
  /// @brief Attempts made in all.
  std::uint32_t attempts_used_{};
  /// @brief Attempts made at the current candidate since it was chosen.
  std::uint32_t attempts_on_candidate_{};
  /// @brief Id the next attempt will get.
  std::uint64_t next_id_{1};
  /// @brief Id of the attempt awaiting its fate; empty when none is.
  std::optional<std::uint64_t> outstanding_id_{};
};

}  // namespace libchiaking::net::protocol::senkusha
