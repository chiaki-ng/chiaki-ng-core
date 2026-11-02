/// @file
/// @brief Declares the Kakusan invitation and reply, the two texts a host
/// and a guest exchange to connect.
#pragma once

#include "libchiaking/detail/kakusan/remembered_devices.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/secret_string.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace libchiaking::kakusan {

/// @brief What an invitation says, in a form a frontend can show before the
/// guest decides to join.
struct InvitationSummary {
  /// @brief The start of the host device's public key, as the invitation
  /// states it. The full key and its fingerprint arrive later, proven, in
  /// GuestVisit::HostIdentified.
  IdentityHint host_hint{};
  /// @brief The host's seats at the moment the invitation was issued. On the
  /// guest's side only capacity is known from the text; the other counts
  /// arrive in GuestVisit::HostIdentified.
  SeatSummary seats;
  /// @brief When the invitation stops being usable, in seconds since the Unix
  /// epoch. Invitations last ten minutes.
  std::uint64_t expires_at{};
  /// @brief The protocol version of the stream the host is sharing.
  std::uint8_t stream_protocol{};
  /// @brief Whether the host looks like a device this one remembers, judged
  /// from host_hint alone and so provisional until
  /// GuestVisit::HostIdentified. Filled in on the guest's side by
  /// Kakusan::read and Kakusan::join; left at
  /// Recognition::Kind::unknown on invitations a host issues.
  Recognition recognition{};

  /// @brief Compares two summaries field by field.
  friend auto operator==(const InvitationSummary&, const InvitationSummary&)
      -> bool = default;
};

/// @brief A host's invitation for one guest: the text to hand over, and its
/// summary.
///
/// A host receives one in SharedSession::InvitationReady and gives text() to
/// the guest by any means outside the library (a message, a QR code). A guest
/// gets one from Kakusan::read and passes it to Kakusan::join.
///
/// The text lets its holder attempt to join, so it is held in a SecretString
/// and wiped on destruction. The type is move-only; clone() copies it
/// deliberately.
class Invitation {
 public:
  /// @brief Longest invitation or reply text, in characters, that the library
  /// accepts.
  static constexpr std::size_t most_text = 131072;

  /// @brief Not copyable; use clone() so each copy of the invitation text is
  /// explicit.
  Invitation(const Invitation&) = delete;
  /// @brief Not copy-assignable; use clone() so each copy of the invitation
  /// text is explicit.
  auto operator=(const Invitation&) -> Invitation& = delete;
  /// @brief Moves the invitation; the source is left without text.
  Invitation(Invitation&&) noexcept = default;
  /// @brief Replaces this invitation with another; the old text is wiped.
  auto operator=(Invitation&&) noexcept -> Invitation& = default;

  /// @brief The invitation as text, to be delivered to the guest.
  /// @return A view valid while this Invitation lives. Copies made from it
  ///     are not wiped by the library.
  auto text() const noexcept -> std::string_view { return text_.view(); }
  /// @brief What the invitation says about the host and the session.
  /// @return The summary, valid while this Invitation lives. It is read once,
  ///     when the invitation is made or read, and does not change.
  auto summary() const noexcept -> const InvitationSummary& { return summary_; }
  /// @brief Makes an independent copy, text included.
  /// @return The copy. Throws `std::bad_alloc` when memory runs out.
  auto clone() const -> Invitation {
    return Invitation{text_.clone(), summary_};
  }

 private:
  friend class Kakusan;
  friend class SharedSession;
  friend class GuestVisit;

  Invitation(SecretString text, InvitationSummary summary) noexcept
      : text_(std::move(text)), summary_(std::move(summary)) {}

  SecretString text_;
  InvitationSummary summary_;
};

/// @brief A guest's answer to an invitation, to be delivered back to the host.
///
/// A guest receives one in GuestVisit::ReplyReady and gives text() to the host
/// by the same means the invitation travelled; the host passes that text to
/// SharedSession::accept_reply. Held in a SecretString and wiped on
/// destruction; move-only, with clone() for a deliberate copy.
class Reply {
 public:
  /// @brief Not copyable; use clone() so each copy of the reply text is
  /// explicit.
  Reply(const Reply&) = delete;
  /// @brief Not copy-assignable; use clone() so each copy of the reply text
  /// is explicit.
  auto operator=(const Reply&) -> Reply& = delete;
  /// @brief Moves the reply; the source is left without text.
  Reply(Reply&&) noexcept = default;
  /// @brief Replaces this reply with another; the old text is wiped.
  auto operator=(Reply&&) noexcept -> Reply& = default;

  /// @brief The reply as text, to be delivered to the host.
  /// @return A view valid while this Reply lives. Copies made from it are not
  ///     wiped by the library.
  auto text() const noexcept -> std::string_view { return text_.view(); }
  /// @brief Makes an independent copy.
  /// @return The copy. Throws `std::bad_alloc` when memory runs out.
  [[nodiscard]] auto clone() const -> Reply { return Reply{text_.clone()}; }

 private:
  friend class GuestVisit;

  explicit Reply(SecretString text) noexcept : text_(std::move(text)) {}

  SecretString text_;
};

}  // namespace libchiaking::kakusan
