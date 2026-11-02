#include "src/core/kakusan/adapters/invitation_book.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::kakusan {

auto InvitationBook::check(const net::kakusan::SealedInvitation& invitation,
                           std::uint64_t wall_seconds)
    -> net::kakusan::Result<void> {
  if (wall_seconds >= invitation.body.invite_expires_at)
    return net::kakusan::fail(net::kakusan::Error::expired);
  if (too_far_ahead(invitation.body.invite_expires_at, wall_seconds))
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  return {};
}

auto InvitationBook::locate(
    const net::kakusan::InvitationId& invitation_id) noexcept -> Entry* {
  return const_cast<Entry*>(std::as_const(*this).locate(invitation_id));
}

auto InvitationBook::locate(const net::kakusan::InvitationId& invitation_id)
    const noexcept -> const Entry* {
  const auto found =
      std::ranges::find_if(entries_, [&invitation_id](const Entry& entry) {
        return entry.invitation.body.invitation_id == invitation_id;
      });
  return found == entries_.end() ? nullptr : &*found;
}

void InvitationBook::expire(std::uint64_t now_us) {
  std::erase_if(entries_, [now_us](const Entry& entry) {
    return entry.status != InvitationStatus::bound &&
           timing::Deadline::elapsed_us(entry.added_us, now_us) >=
               invitation_lifetime_us;
  });
}

auto InvitationBook::add(const net::kakusan::SealedInvitation& invitation,
                         std::uint64_t now_us) -> net::kakusan::Result<void> {
  expire(now_us);
  if (locate(invitation.body.invitation_id) != nullptr)
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  if (entries_.size() >= max_tracked_invitations)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  entries_.push_back(
      Entry{invitation.clone(), now_us, InvitationStatus::offered, {}});
  return {};
}

auto InvitationBook::import_reply(const net::kakusan::SealedReply& reply,
                                  std::uint64_t wall_seconds,
                                  std::uint64_t now_us)
    -> net::kakusan::Result<ImportOutcome> {
  const auto outcome = bind_reply(reply, wall_seconds, now_us);
  expire(now_us);
  return outcome;
}

auto InvitationBook::answered_by(const net::kakusan::SealedReply& reply)
    const noexcept -> std::optional<net::kakusan::InvitationId> {
  const auto found =
      std::ranges::find_if(entries_, [&reply](const Entry& entry) {
        return net::kakusan::ShortBlockCodec::answers(entry.invitation, reply);
      });
  if (found == entries_.end()) return std::nullopt;
  return found->invitation.body.invitation_id;
}

auto InvitationBook::bind_reply(const net::kakusan::SealedReply& reply,
                                std::uint64_t wall_seconds,
                                std::uint64_t now_us)
    -> net::kakusan::Result<ImportOutcome> {
  const auto invitation_id = answered_by(reply);
  auto* entry = invitation_id ? locate(*invitation_id) : nullptr;
  if (entry == nullptr || entry->status == InvitationStatus::consumed)
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  if (entry->status == InvitationStatus::bound) {
    if (!memory::constant_time_equal(entry->reply_hash, reply.hash))
      return net::kakusan::fail(net::kakusan::Error::invalid_block);
    return ImportOutcome::already_bound;
  }

  if (!pairing_allowed(now_us))
    return net::kakusan::fail(net::kakusan::Error::pairing_failed);
  const auto& offered = entry->invitation.body;
  if (wall_seconds >= offered.invite_expires_at ||
      timing::Deadline::elapsed_us(entry->added_us, now_us) >=
          invitation_lifetime_us)
    return net::kakusan::fail(net::kakusan::Error::expired);
  if (too_far_ahead(offered.invite_expires_at, wall_seconds))
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  if (pending_count() >= max_pending_attempts)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);

  entry->status = InvitationStatus::bound;
  entry->reply_hash = reply.hash;
  return ImportOutcome::bound;
}

auto InvitationBook::fail(const net::kakusan::InvitationId& invitation_id,
                          std::uint64_t now_us) -> net::kakusan::Result<void> {
  auto* entry = locate(invitation_id);
  if (entry == nullptr)
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  entry->status = InvitationStatus::consumed;
  expire(now_us);
  return {};
}

auto InvitationBook::fail_authentication(
    const net::kakusan::InvitationId& invitation_id, std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  const auto failed = fail(invitation_id, now_us);
  if (failed) record_authentication_failure(now_us);
  return failed;
}

auto InvitationBook::finish(const net::kakusan::InvitationId& invitation_id)
    -> net::kakusan::Result<void> {
  auto* entry = locate(invitation_id);
  if (entry == nullptr || entry->status == InvitationStatus::offered)
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  entry->status = InvitationStatus::consumed;
  return {};
}

void InvitationBook::record_authentication_failure(
    std::uint64_t now_us) noexcept {
  failures_[failure_next_] = now_us;
  failure_next_ = (failure_next_ + 1) % failures_.size();
  failure_count_ = std::min(failure_count_ + 1, failures_.size());
  if (failure_count_ < cooldown_failure_threshold) return;
  const auto oldest = failures_[failure_next_];
  if (timing::Deadline::elapsed_us(oldest, now_us) < cooldown_window_us)
    cooldown_until_us_ = now_us + cooldown_duration_us;
}

auto InvitationBook::pairing_allowed(std::uint64_t now_us) const noexcept
    -> bool {
  return now_us >= cooldown_until_us_;
}

auto InvitationBook::status(const net::kakusan::InvitationId& invitation_id)
    const noexcept -> std::optional<InvitationStatus> {
  const auto* entry = locate(invitation_id);
  if (entry == nullptr) return std::nullopt;
  return entry->status;
}

auto InvitationBook::find(const net::kakusan::InvitationId& invitation_id)
    const noexcept -> const net::kakusan::SealedInvitation* {
  const auto* entry = locate(invitation_id);
  return entry == nullptr ? nullptr : &entry->invitation;
}

auto InvitationBook::pending_count() const noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count(entries_, InvitationStatus::bound, &Entry::status));
}

}  // namespace libchiaking::kakusan
