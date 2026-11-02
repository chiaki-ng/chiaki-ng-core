/// @file
/// @brief Declares InvitationBook, the host's record of the invitations it has
/// issued and the replies bound to them, with the limits and cooldown that
/// protect verification from guessing.
#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/kakusan/model/clock.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::kakusan {

/// @brief How long an invitation is honoured after it is issued, in
/// monotonic microseconds: ten minutes.
inline constexpr std::uint64_t invitation_lifetime_us =
    net::kakusan::invitation_lifetime_seconds * microseconds_per_second;
/// @brief How far, in seconds, an invitation's expiry may lie beyond a full
/// lifetime from now before it is taken as forged rather than as clock skew
/// between the two devices.
inline constexpr std::uint64_t clock_skew_tolerance_seconds = 120;
/// @brief Most invitations that may have a reply bound and still be
/// connecting at once.
inline constexpr std::size_t max_pending_attempts = 8;
/// @brief Most invitations the book holds at once.
inline constexpr std::size_t max_tracked_invitations = 64;
/// @brief How many failed verifications within cooldown_window_us start a
/// cooldown.
inline constexpr std::size_t cooldown_failure_threshold = 3;
/// @brief The window, in monotonic microseconds (ten minutes), in which
/// cooldown_failure_threshold failures start a cooldown.
inline constexpr std::uint64_t cooldown_window_us =
    600 * microseconds_per_second;
/// @brief How long a cooldown refuses new replies and codes, in monotonic
/// microseconds: five minutes.
inline constexpr std::uint64_t cooldown_duration_us =
    300 * microseconds_per_second;

/// @brief What importing a valid reply did.
enum class ImportOutcome : std::uint8_t {
  /// @brief The reply was bound to its invitation for the first time.
  bound,
  /// @brief The same reply was already bound to its invitation; nothing
  /// changed.
  already_bound,
};

/// @brief Where an invitation in the book stands.
enum class InvitationStatus : std::uint8_t {
  /// @brief Issued and waiting for a reply.
  offered,
  /// @brief A reply is bound to it and the guest is connecting; it no
  /// longer expires.
  bound,
  /// @brief Used up: the guest was admitted or failed, so no reply is
  /// accepted for it again; dropped once its lifetime is over.
  consumed,
};

/// @brief The host's invitations, keyed by invitation id, and its recent
/// verification failures.
///
/// Each invitation admits one guest: once a reply is bound, only that same
/// reply is accepted again, and once consumed no reply is. Three failed
/// verifications within ten minutes refuse new replies and codes for five
/// minutes. Ages are measured on the monotonic clock from when the invitation
/// was added; expiry times written in invitations are wall-clock seconds.
///
/// Owned by SharedSession::Core and used under its lock; not thread-safe.
class InvitationBook {
 public:
  /// @brief Checks an invitation's expiry against the wall clock, as a guest
  /// does before accepting it.
  ///
  /// @param invitation The invitation.
  /// @param wall_seconds The current wall-clock time, in seconds since the
  ///     epoch.
  /// @return Nothing when it is still valid.
  /// @retval net::kakusan::Error::expired Its expiry time has passed.
  /// @retval net::kakusan::Error::invalid_block Its expiry lies further ahead
  ///     than a lifetime plus clock_skew_tolerance_seconds.
  [[nodiscard]] static auto check(
      const net::kakusan::SealedInvitation& invitation,
      std::uint64_t wall_seconds) -> net::kakusan::Result<void>;

  /// @brief Records a newly issued invitation, after dropping expired ones.
  ///
  /// @param invitation The invitation; a copy is kept.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once it is recorded as offered.
  /// @retval net::kakusan::Error::invalid_block An invitation with the same
  ///     id is already recorded.
  /// @retval net::kakusan::Error::no_capacity The book holds
  ///     max_tracked_invitations already.
  [[nodiscard]] auto add(const net::kakusan::SealedInvitation& invitation,
                         std::uint64_t now_us) -> net::kakusan::Result<void>;

  /// @brief Binds a guest's reply to the invitation it answers, then drops
  /// expired invitations.
  ///
  /// @param reply The reply, already parsed and authenticated.
  /// @param wall_seconds The current wall-clock time, in seconds since the
  ///     epoch.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Whether the reply was bound now or had been already.
  /// @retval net::kakusan::Error::invalid_block The reply answers no recorded
  ///     invitation, its invitation is consumed, a different reply is already
  ///     bound to it, or its expiry is implausibly far ahead.
  /// @retval net::kakusan::Error::pairing_failed A cooldown is in force.
  /// @retval net::kakusan::Error::expired The invitation's wall-clock expiry
  ///     or monotonic lifetime has passed.
  /// @retval net::kakusan::Error::no_capacity max_pending_attempts replies
  ///     are already bound.
  [[nodiscard]] auto import_reply(const net::kakusan::SealedReply& reply,
                                  std::uint64_t wall_seconds,
                                  std::uint64_t now_us)
      -> net::kakusan::Result<ImportOutcome>;

  /// @brief Finds the invitation a reply answers.
  /// @param reply The reply.
  /// @return The invitation's id, or nothing when it answers none recorded.
  [[nodiscard]] auto answered_by(const net::kakusan::SealedReply& reply)
      const noexcept -> std::optional<net::kakusan::InvitationId>;

  /// @brief Marks an invitation consumed because its guest failed, then drops
  /// expired invitations.
  /// @param invitation_id The invitation.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once it is consumed.
  /// @retval net::kakusan::Error::invalid_block No such invitation is
  ///     recorded.
  [[nodiscard]] auto fail(const net::kakusan::InvitationId& invitation_id,
                          std::uint64_t now_us) -> net::kakusan::Result<void>;
  /// @brief Marks an invitation consumed because verification failed, and
  /// counts the failure towards a cooldown.
  /// @param invitation_id The invitation.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing once it is consumed and counted.
  /// @retval net::kakusan::Error::invalid_block No such invitation is
  ///     recorded; nothing is counted.
  [[nodiscard]] auto fail_authentication(
      const net::kakusan::InvitationId& invitation_id, std::uint64_t now_us)
      -> net::kakusan::Result<void>;
  /// @brief Marks an invitation consumed because its guest's visit is over.
  /// @param invitation_id The invitation.
  /// @return Nothing once it is consumed.
  /// @retval net::kakusan::Error::invalid_block No such invitation is
  ///     recorded, or it has no reply bound yet.
  [[nodiscard]] auto finish(const net::kakusan::InvitationId& invitation_id)
      -> net::kakusan::Result<void>;

  /// @brief Counts one failed verification, starting a cooldown of
  /// cooldown_duration_us when it is the third within cooldown_window_us.
  /// @param now_us The current monotonic time, in microseconds.
  void record_authentication_failure(std::uint64_t now_us) noexcept;
  /// @brief Tells whether replies and codes may be accepted now.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return False while a cooldown is in force.
  auto pairing_allowed(std::uint64_t now_us) const noexcept -> bool;

  /// @brief Drops every invitation that is not bound and was added a lifetime
  /// or more ago.
  /// @param now_us The current monotonic time, in microseconds.
  void expire(std::uint64_t now_us);

  /// @brief Where an invitation stands.
  /// @param invitation_id The invitation.
  /// @return Its status, or nothing when it is not recorded.
  [[nodiscard]] auto status(const net::kakusan::InvitationId& invitation_id)
      const noexcept -> std::optional<InvitationStatus>;
  /// @brief Looks up an issued invitation.
  /// @param invitation_id The invitation.
  /// @return The invitation as issued, or null when it is not recorded; valid
  ///     until the book next changes.
  auto find(const net::kakusan::InvitationId& invitation_id) const noexcept
      -> const net::kakusan::SealedInvitation*;
  /// @brief Counts the invitations with a reply bound.
  /// @return The number of guests connecting.
  auto pending_count() const noexcept -> std::size_t;
  /// @brief Counts every recorded invitation, whatever its status.
  /// @return The number held.
  auto tracked_count() const noexcept -> std::size_t { return entries_.size(); }

 private:
  /// @brief One recorded invitation.
  struct Entry {
    /// @brief The invitation as issued.
    net::kakusan::SealedInvitation invitation;
    /// @brief When it was added, in monotonic microseconds.
    std::uint64_t added_us{};
    /// @brief Where it stands.
    InvitationStatus status{InvitationStatus::offered};
    /// @brief The hash of the bound reply; meaningful once the status is
    /// not offered.
    net::kakusan::Digest256 reply_hash{};
  };

  /// @brief Tells whether an expiry time lies implausibly far ahead.
  /// @param invite_expires_at The expiry time, in wall-clock seconds.
  /// @param wall_seconds The current wall-clock time, in seconds.
  /// @return True when it is more than a lifetime plus
  ///     clock_skew_tolerance_seconds away.
  static constexpr auto too_far_ahead(std::uint64_t invite_expires_at,
                                      std::uint64_t wall_seconds) noexcept
      -> bool {
    return invite_expires_at > wall_seconds &&
           invite_expires_at - wall_seconds >
               net::kakusan::invitation_lifetime_seconds +
                   clock_skew_tolerance_seconds;
  }

  /// @brief Does the work of import_reply() without the final expiry pass.
  /// @param reply The reply.
  /// @param wall_seconds The current wall-clock time, in seconds.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return As import_reply().
  [[nodiscard]] auto bind_reply(const net::kakusan::SealedReply& reply,
                                std::uint64_t wall_seconds,
                                std::uint64_t now_us)
      -> net::kakusan::Result<ImportOutcome>;
  /// @brief Finds an entry by invitation id.
  /// @param invitation_id The invitation.
  /// @return The entry, or null.
  auto locate(const net::kakusan::InvitationId& invitation_id) noexcept
      -> Entry*;
  /// @brief Finds an entry by invitation id.
  /// @param invitation_id The invitation.
  /// @return The entry, or null.
  auto locate(const net::kakusan::InvitationId& invitation_id) const noexcept
      -> const Entry*;

  /// @brief The recorded invitations, at most max_tracked_invitations.
  std::vector<Entry> entries_;
  /// @brief When the last failures happened, in monotonic microseconds; a
  /// ring indexed by failure_next_.
  std::array<std::uint64_t, cooldown_failure_threshold> failures_{};
  /// @brief How many slots of failures_ hold a failure.
  std::size_t failure_count_{};
  /// @brief The slot the next failure goes in, which holds the oldest one.
  std::size_t failure_next_{};
  /// @brief When the current cooldown ends, in monotonic microseconds; zero
  /// or past when none is in force.
  std::uint64_t cooldown_until_us_{};
};

}  // namespace libchiaking::kakusan
