/// @file
/// @brief Declares the value types of Kakusan: identifiers, progress,
/// refusals, seats, codes and sharing rules.
#pragma once

#include "libchiaking/detail/kakusan/protocol_reason.hpp"
#include "libchiaking/detail/secret_array.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::kakusan {

/// @brief A fixed-size opaque identifier, typed so that different kinds cannot
/// be mixed up.
/// @tparam Tag An empty type that distinguishes one kind of identifier from
///     another.
/// @tparam Size The identifier's length in bytes.
template <class Tag, std::size_t Size>
class Identifier {
 public:
  /// @brief The identifier's length in bytes.
  static constexpr std::size_t size = Size;
  /// @brief The raw form of the identifier.
  using Bytes = std::array<std::byte, Size>;

  /// @brief Makes the all-zero identifier, which names nothing.
  constexpr Identifier() noexcept = default;
  /// @brief Makes an identifier from its raw bytes.
  /// @param bytes The bytes to adopt.
  constexpr explicit Identifier(const Bytes& bytes) noexcept : bytes_(bytes) {}

  /// @brief The raw bytes, for storing or displaying.
  /// @return The bytes, all zero for a default-made identifier.
  constexpr auto bytes() const noexcept -> const Bytes& { return bytes_; }
  /// @brief Tells whether two identifiers are the same bytes.
  constexpr bool operator==(const Identifier&) const noexcept = default;

 private:
  Bytes bytes_{};
};

/// @brief Identifies one guest within one shared session. It is created with
/// the invitation and stays with whoever answers it.
using GuestId = Identifier<struct GuestIdTag, 16>;
/// @brief A device's Kakusan public key (Ed25519). This is what a device is
/// known by.
using IdentityKey = Identifier<struct IdentityKeyTag, 32>;
/// @brief The SHA-256 digest of an IdentityKey, shown so two people can compare
/// devices.
using Fingerprint = Identifier<struct FingerprintTag, 32>;
/// @brief The first eight bytes of an IdentityKey, as carried in an invitation
/// or reply. It says which device the text claims to come from; the claim is
/// only proven once the two devices are connected.
using IdentityHint = Identifier<struct IdentityHintTag, 8>;

/// @brief The stage one guest's visit has reached, seen the same way from both
/// sides.
///
/// The stages are passed in the order listed, except that any stage can go
/// straight to leaving or gone.
enum class Progress : std::uint8_t {
  /// @brief An invitation exists; the reply has not yet been accepted.
  waiting,
  /// @brief The reply is in and the two devices are establishing a direct
  /// connection.
  connecting,
  /// @brief Connected; each side shows a code and waits for the other's to be
  /// entered.
  verifying,
  /// @brief Verified. The guest is admitted and waits for the host to give a
  /// seat and the console to accept it.
  waiting_for_seat,
  /// @brief The console has seated the guest and the guest is setting up the
  /// stream and controller.
  getting_ready,
  /// @brief The guest is receiving the stream and their controller reaches
  /// the console.
  playing,
  /// @brief The visit is ending and the other side is being told.
  leaving,
  /// @brief The visit is over. This is final.
  gone,
};

/// @brief Why a guest's visit ended once it was under way.
/// @see Refusal for failures.
class EndReason {
 public:
  /// @brief The possible reasons.
  enum class Kind : std::uint8_t {
    /// @brief A person ended it: the guest left, or the host removed them.
    left,
    /// @brief The guest's seat was withdrawn because the host reduced the
    /// seats.
    no_seat,
    /// @brief The shared session, or the guest's place on the console,
    /// ended.
    session_ended,
    /// @brief The other side stopped responding in time.
    timed_out,
    /// @brief The connection between the devices failed.
    connection_lost,
    /// @brief The protocol's rules were broken.
    protocol,
  };

  /// @brief Makes an end reason of a given kind.
  /// @param kind The reason.
  constexpr EndReason(Kind kind) noexcept : kind_(kind) {}
  /// @brief Translates the reason sent over the protocol.
  /// @param reason The protocol's reason.
  constexpr explicit EndReason(ProtocolReason reason) noexcept
      : kind_(told(reason)) {}

  /// @brief Which of the reasons this is.
  /// @return The kind; name() gives it as text.
  constexpr auto kind() const noexcept -> Kind { return kind_; }
  /// @brief The reason's upper-case name, for logs.
  /// @return A string with static lifetime, such as "NO_SEAT".
  constexpr auto name() const noexcept -> std::string_view {
    switch (kind_) {
      case Kind::left:
        return "LEFT";
      case Kind::no_seat:
        return "NO_SEAT";
      case Kind::session_ended:
        return "SESSION_ENDED";
      case Kind::timed_out:
        return "TIMED_OUT";
      case Kind::connection_lost:
        return "CONNECTION_LOST";
      case Kind::protocol:
        break;
    }
    return "PROTOCOL";
  }

  /// @brief Tells whether two end reasons are the same kind.
  constexpr bool operator==(const EndReason&) const noexcept = default;

 private:
  static constexpr auto told(ProtocolReason reason) noexcept
      -> Kind {
    using Told = ProtocolReason;
    switch (reason) {
      case Told::user:
        return Kind::left;
      case Told::capacity:
        return Kind::no_seat;
      case Told::source:
        return Kind::session_ended;
      case Told::timeout:
        return Kind::timed_out;
      case Told::network:
        return Kind::connection_lost;
      case Told::protocol:
        break;
    }
    return Kind::protocol;
  }

  Kind kind_;
};

/// @brief Why Kakusan refused a request or a visit failed.
///
/// Returned as the error of most Kakusan calls and carried in events when a
/// guest's visit fails.
class Refusal {
 public:
  /// @brief The possible causes.
  enum class Kind : std::uint8_t {
    /// @brief An invitation or reply text is not valid, is too long, or
    /// does not belong to this session.
    invalid_text,
    /// @brief The invitation's ten minutes have passed.
    expired,
    /// @brief The other device speaks a protocol version this one does not,
    /// or the host's stream protocol version cannot be forwarded.
    unsupported_version,
    /// @brief A device presented a different identity from the one
    /// expected.
    identity_changed,
    /// @brief The two devices do not agree on what was exchanged, so
    /// something altered it on the way.
    tampered,
    /// @brief The verification codes did not match, or too many attempts
    /// failed recently and verification is paused.
    verification_failed,
    /// @brief No direct network path between the two devices could be
    /// established.
    unreachable,
    /// @brief No seat is free for the guest. Also returned by
    /// SharedSession::invite_player when too many invitations are
    /// outstanding.
    no_free_seat,
    /// @brief The console did not let the guest take the seat.
    console_refused_seat,
    /// @brief A message from the other device could not be decoded.
    malformed,
    /// @brief A step did not complete in time.
    timed_out,
    /// @brief The other device broke the protocol's rules, or a controller
    /// state could not be carried.
    protocol,
    /// @brief The host's stream is in a form that cannot be forwarded to
    /// guests.
    stream_not_shareable,
    /// @brief The connection to the guest could not keep up with the
    /// stream.
    connection_too_slow,
    /// @brief The session being shared has ended.
    session_ended,
    /// @brief Something failed inside the library or its crypto provider,
    /// or a call named a guest or a stage that does not apply.
    internal,
    /// @brief A value passed to a call was not acceptable.
    invalid_argument,
    /// @brief The SharedSession is no longer sharing, or the stream has not
    /// started being shared yet.
    not_sharing,
    /// @brief The play::Session is already being shared.
    busy,
    /// @brief The play::Session is of a kind that cannot be shared: a cloud
    /// session, or one with an unsupported stream protocol version.
    cannot_be_shared,
    /// @brief The GuestVisit is leaving or has ended.
    not_visiting,
  };

  /// @brief Makes a refusal of a given kind.
  /// @param kind The cause.
  constexpr Refusal(Kind kind) noexcept : kind_(kind) {}

  /// @brief Which of the causes this is.
  /// @return The kind; name() gives it as text and code() as a number.
  constexpr auto kind() const noexcept -> Kind { return kind_; }
  /// @brief The cause's upper-case name, for logs.
  /// @return A string with static lifetime, such as "NO_FREE_SEAT".
  constexpr auto name() const noexcept -> std::string_view {
    switch (kind_) {
      case Kind::invalid_text:
        return "INVALID_TEXT";
      case Kind::expired:
        return "EXPIRED";
      case Kind::unsupported_version:
        return "UNSUPPORTED_VERSION";
      case Kind::identity_changed:
        return "IDENTITY_CHANGED";
      case Kind::tampered:
        return "TAMPERED";
      case Kind::verification_failed:
        return "VERIFICATION_FAILED";
      case Kind::unreachable:
        return "UNREACHABLE";
      case Kind::no_free_seat:
        return "NO_FREE_SEAT";
      case Kind::console_refused_seat:
        return "CONSOLE_REFUSED_SEAT";
      case Kind::malformed:
        return "MALFORMED";
      case Kind::timed_out:
        return "TIMED_OUT";
      case Kind::protocol:
        return "PROTOCOL";
      case Kind::stream_not_shareable:
        return "STREAM_NOT_SHAREABLE";
      case Kind::connection_too_slow:
        return "CONNECTION_TOO_SLOW";
      case Kind::session_ended:
        return "SESSION_ENDED";
      case Kind::invalid_argument:
        return "INVALID_ARGUMENT";
      case Kind::not_sharing:
        return "NOT_SHARING";
      case Kind::busy:
        return "BUSY";
      case Kind::cannot_be_shared:
        return "CANNOT_BE_SHARED";
      case Kind::not_visiting:
        return "NOT_VISITING";
      case Kind::internal:
        break;
    }
    return "INTERNAL";
  }
  /// @brief A stable number for the cause, for logs and exit codes.
  /// @return 1 for Kind::invalid_argument, 3 to 17 for the causes that come
  ///     from the protocol, 19 for Kind::internal, and 30 to 33 for
  ///     Kind::not_sharing, Kind::busy, Kind::cannot_be_shared and
  ///     Kind::not_visiting.
  /// @see VisitOutcome::code
  constexpr auto code() const noexcept -> int {
    switch (kind_) {
      case Kind::invalid_argument:
        return 1;
      case Kind::invalid_text:
        return 3;
      case Kind::expired:
        return 4;
      case Kind::unsupported_version:
        return 5;
      case Kind::identity_changed:
        return 6;
      case Kind::tampered:
        return 7;
      case Kind::verification_failed:
        return 8;
      case Kind::unreachable:
        return 9;
      case Kind::no_free_seat:
        return 10;
      case Kind::console_refused_seat:
        return 11;
      case Kind::malformed:
        return 12;
      case Kind::timed_out:
        return 13;
      case Kind::protocol:
        return 14;
      case Kind::stream_not_shareable:
        return 15;
      case Kind::connection_too_slow:
        return 16;
      case Kind::session_ended:
        return 17;
      case Kind::not_sharing:
        return 30;
      case Kind::busy:
        return 31;
      case Kind::cannot_be_shared:
        return 32;
      case Kind::not_visiting:
        return 33;
      case Kind::internal:
        break;
    }
    return 19;
  }

  /// @brief Tells whether two refusals have the same cause.
  constexpr bool operator==(const Refusal&) const noexcept = default;

 private:
  Kind kind_;
};

/// @brief One controller button as a bit, for building the mask of buttons
/// guests may press.
///
/// Combine the values with bitwise or after casting to `std::uint32_t`.
///
/// @see SharingRules::allowed_buttons
enum class Button : std::uint32_t {
  /// @brief The bottom face button (cross).
  south = 1U << 0U,
  /// @brief The right face button (circle).
  east = 1U << 1U,
  /// @brief The left face button (square).
  west = 1U << 2U,
  /// @brief The top face button (triangle).
  north = 1U << 3U,
  /// @brief Directional pad up.
  dpad_up = 1U << 4U,
  /// @brief Directional pad down.
  dpad_down = 1U << 5U,
  /// @brief Directional pad left.
  dpad_left = 1U << 6U,
  /// @brief Directional pad right.
  dpad_right = 1U << 7U,
  /// @brief The left shoulder button.
  l1 = 1U << 8U,
  /// @brief The right shoulder button.
  r1 = 1U << 9U,
  /// @brief Pressing the left stick.
  l3 = 1U << 10U,
  /// @brief Pressing the right stick.
  r3 = 1U << 11U,
  /// @brief The options button.
  options = 1U << 12U,
  /// @brief The share (create) button.
  share = 1U << 13U,
  /// @brief The home (PS) button.
  home = 1U << 14U,
  /// @brief Pressing the touchpad.
  touchpad = 1U << 15U,
  /// @brief The left trigger.
  l2 = 1U << 16U,
  /// @brief The right trigger.
  r2 = 1U << 17U,
};

/// @brief The mask with every Button set.
inline constexpr std::uint32_t all_buttons = 0x3ffffU;
/// @brief The buttons a host must always allow: every Button except
/// Button::share and Button::home.
inline constexpr std::uint32_t required_buttons =
    all_buttons & ~(static_cast<std::uint32_t>(Button::share) |
                    static_cast<std::uint32_t>(Button::home));

/// @brief A guest's claim on one Kakusan seat.
struct SeatGrant {
  /// @brief The seat's number within the shared session's seats. It is not
  /// the console's seat number; see PlayerSummary::console_seat for that.
  std::uint8_t number{};
  /// @brief Tells successive grants of the same seat number apart; it changes
  /// each time the seat is given out.
  std::uint32_t generation{};

  /// @brief Tells whether two grants are the same seat and generation.
  constexpr bool operator==(const SeatGrant&) const noexcept = default;
};

/// @brief How a shared session's seats are divided at one moment.
struct SeatSummary {
  /// @brief The total number of seats, for the host's side and guests
  /// together.
  std::uint8_t capacity{};
  /// @brief Seats kept for people playing at the host's own device.
  std::uint8_t local_seats{};
  /// @brief Seats currently granted to remote guests.
  std::uint8_t remote_seats{};
  /// @brief Seats being held for guests who are in the middle of being
  /// seated.
  std::uint8_t reserved_seats{};

  /// @brief Tells whether two summaries hold the same counts.
  constexpr bool operator==(const SeatSummary&) const noexcept = default;
};

/// @brief The four-digit code each side reads aloud so the two people can
/// confirm they are connected to each other and not to someone in between.
///
/// The digits are kept in wiping storage and zeroed when the code is destroyed
/// or moved from. The type is move-only.
class VerificationCode {
 public:
  /// @brief The number of digits.
  static constexpr std::size_t size = 4;

  /// @brief Makes a code from four characters.
  /// @param digits The characters to adopt. They are not checked; use parse()
  ///     for text a person typed.
  explicit VerificationCode(std::span<const char, size> digits) noexcept {
    std::ranges::copy(digits, digits_.begin());
  }

  /// @brief Makes a code from text a person typed.
  /// @param digits The text to check.
  /// @return The code, or an empty optional unless the text is exactly four
  ///     ASCII digits.
  [[nodiscard]] static auto parse(std::string_view digits) noexcept
      -> std::optional<VerificationCode> {
    const auto digit = [](char value) { return value >= '0' && value <= '9'; };
    if (digits.size() != size || !std::ranges::all_of(digits, digit))
      return std::nullopt;
    return VerificationCode{std::span<const char, size>{digits.data(), size}};
  }

  /// @brief The four digits as text, for showing or reading out.
  /// @return A view valid while this VerificationCode lives; not
  ///     null-terminated.
  auto digits() const noexcept -> std::string_view {
    return {digits_.data(), size};
  }

 private:
  SecretArray<size, char> digits_;
};

/// @brief The host's terms for sharing: how many seats there are, how many the
/// host keeps, and which buttons guests may press.
///
/// Only valid combinations exist; obtain one from create().
class SharingRules {
 public:
  /// @brief The largest capacity a session can be shared with.
  static constexpr std::uint8_t most_seats = 4;

  /// @brief Makes a set of rules if the values are a valid combination.
  /// @param capacity Total seats, from 1 to most_seats.
  /// @param local_seats Seats kept for the host's own device, from 1 to
  ///     capacity. The rest are available to guests.
  /// @param allowed_buttons Mask of Button values guests may press. It must
  ///     include all of required_buttons and nothing outside all_buttons;
  ///     presses of buttons left out are dropped before they reach the
  ///     console.
  /// @return The rules, or an empty optional when any value is out of range.
  [[nodiscard]] static constexpr auto create(
      std::uint8_t capacity, std::uint8_t local_seats,
      std::uint32_t allowed_buttons = all_buttons) noexcept
      -> std::optional<SharingRules> {
    if (capacity < 1 || capacity > most_seats || local_seats < 1 ||
        local_seats > capacity || (allowed_buttons & ~all_buttons) != 0 ||
        (allowed_buttons & required_buttons) != required_buttons)
      return std::nullopt;
    return SharingRules{capacity, local_seats, allowed_buttons};
  }

  /// @brief Total seats, for the host's side and guests together.
  /// @return From 1 to most_seats.
  constexpr auto capacity() const noexcept -> std::uint8_t { return capacity_; }
  /// @brief Seats kept for people playing at the host's own device.
  /// @return From 1 to capacity(); the difference is open to guests.
  constexpr auto local_seats() const noexcept -> std::uint8_t {
    return local_seats_;
  }
  /// @brief Mask of Button values guests may press.
  /// @return A mask that holds all of required_buttons and nothing outside
  ///     all_buttons.
  constexpr auto allowed_buttons() const noexcept -> std::uint32_t {
    return allowed_buttons_;
  }

  /// @brief Tells whether two sets of rules are the same.
  constexpr bool operator==(const SharingRules&) const noexcept = default;

 private:
  constexpr SharingRules(std::uint8_t capacity, std::uint8_t local_seats,
                         std::uint32_t allowed_buttons) noexcept
      : capacity_(capacity),
        local_seats_(local_seats),
        allowed_buttons_(allowed_buttons) {}

  std::uint8_t capacity_;
  std::uint8_t local_seats_;
  std::uint32_t allowed_buttons_;
};

}  // namespace libchiaking::kakusan
