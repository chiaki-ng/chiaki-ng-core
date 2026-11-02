/// @file
/// @brief Declares the values of Takion's SCTP-style association handshake:
/// tags, sequence numbers, windows, stream counts, the cookie, and what an
/// established association remembers.
///
/// The client sends INIT with verification tag 0 in the message header and
/// its own tag, window, stream counts and initial sequence number in the
/// chunk; the console answers INIT_ACK addressed to that tag, carrying its
/// own values and a 32-byte cookie; the client echoes the cookie in
/// COOKIE_ECHO; the console confirms with COOKIE_ACK. Every value is
/// big-endian on the wire.
#pragma once

#include "src/core/net/transport/datagram/udp_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace libchiaking::net::protocol::takion {

/// @brief Bytes of the opaque cookie the console issues in INIT_ACK.
inline constexpr std::size_t handshake_cookie_size = 32;
/// @brief Receive window, in bytes, the client advertises: the socket's
/// receive buffer, 0x19000 (102400).
inline constexpr std::uint32_t default_advertised_receive_window =
    net::transport::datagram::socket_receive_window;
/// @brief Outbound streams the client asks for.
inline constexpr std::uint16_t default_outbound_stream_count = 100;
/// @brief Inbound streams the client asks for.
inline constexpr std::uint16_t default_inbound_stream_count = 100;
/// @brief Times INIT, and separately COOKIE_ECHO, are sent before the
/// handshake fails (100): each is resent every 100 ms, so a lost INIT or
/// COOKIE_ECHO costs a tenth of a second rather than the session.
inline constexpr std::uint32_t default_handshake_attempts = 100;
/// @brief How long each INIT or COOKIE_ECHO waits for its answer, in
/// microseconds (100 ms).
inline constexpr std::uint64_t default_handshake_response_timeout_us = 100'000;

/// @brief A 32-bit association tag: each side picks its own and the other side
/// puts it in the message header of every packet it sends, so stale or
/// foreign packets can be told apart. 0 is reserved for INIT.
struct VerificationTag {
  /// @brief The tag.
  std::uint32_t value{};

  /// @brief Compares the tags.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const VerificationTag&) const noexcept = default;
};

/// @brief The first reliable data sequence number a side will use.
struct InitialSequenceNumber {
  /// @brief The sequence number.
  std::uint32_t value{};

  /// @brief Compares the numbers.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const InitialSequenceNumber&) const noexcept =
      default;
};

/// @brief Bytes a side says it can buffer (SCTP's a_rwnd).
struct AdvertisedReceiveWindow {
  /// @brief The window, in bytes.
  std::uint32_t value{default_advertised_receive_window};

  /// @brief Compares the windows.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const AdvertisedReceiveWindow&) const noexcept =
      default;
};

/// @brief A number of streams offered or accepted.
struct StreamCount {
  /// @brief The count.
  std::uint16_t value{default_outbound_stream_count};

  /// @brief Compares the counts.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const StreamCount&) const noexcept = default;
};

/// @brief The console's opaque handshake cookie.
using HandshakeCookie = std::array<std::byte, handshake_cookie_size>;

/// @brief The 16 bytes an INIT or INIT_ACK chunk carries, in wire order: tag,
/// window (u32), outbound streams, inbound streams (u16 each), initial
/// sequence number (u32).
struct InitValues {
  /// @brief The sender's own tag.
  VerificationTag tag{};
  /// @brief The sender's receive window.
  AdvertisedReceiveWindow advertised_receive_window{};
  /// @brief The sender's outbound stream count.
  StreamCount outbound_streams{};
  /// @brief The sender's inbound stream count.
  StreamCount inbound_streams{};
  /// @brief The sender's initial sequence number.
  InitialSequenceNumber initial_sequence_number{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const InitValues&) const noexcept = default;
};

/// @brief An INIT_ACK's 48 bytes: the console's INIT values and its cookie.
struct InitAcknowledgementValues {
  /// @brief The console's values.
  InitValues init{};
  /// @brief The cookie to echo.
  HandshakeCookie cookie{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const InitAcknowledgementValues&) const noexcept =
      default;
};

/// @brief Where a handshake is, as a snapshot reports it.
enum class HandshakeState : std::uint8_t {
  /// @brief Not started.
  idle,
  /// @brief About to send INIT; not reported by the current machine.
  send_init,
  /// @brief INIT sent; waiting for INIT_ACK.
  await_init_acknowledgement,
  /// @brief Checking an INIT_ACK; not reported by the current machine.
  validate_init_acknowledgement_and_cookie,
  /// @brief About to send COOKIE_ECHO; not reported by the current machine.
  send_cookie,
  /// @brief COOKIE_ECHO sent; waiting for COOKIE_ACK.
  await_cookie_acknowledgement,
  /// @brief The association is up.
  connected,
  /// @brief The handshake failed or was cancelled.
  failed,
};

/// @brief How patiently the handshake resends.
struct HandshakeRetryPolicy {
  /// @brief Sends of each of INIT and COOKIE_ECHO before giving up; the count
  /// restarts when INIT_ACK arrives.
  std::uint32_t maximum_attempts{default_handshake_attempts};
  /// @brief Wait for each answer, in microseconds.
  std::uint64_t response_timeout_microseconds{
      default_handshake_response_timeout_us};
};

/// @brief When the outstanding INIT or COOKIE_ECHO is due to be resent.
struct HandshakeDeadline {
  /// @brief The deadline, in monotonic microseconds; 0 when none is armed.
  std::uint64_t monotonic_microseconds{};
};

/// @brief What both sides agreed: the association a Takion session runs on.
struct EstablishedHandshake {
  /// @brief Our tag; the console's packets must carry it.
  VerificationTag local_tag{};
  /// @brief The console's tag; our packets carry it.
  VerificationTag remote_tag{};
  /// @brief Our first reliable sequence number.
  InitialSequenceNumber local_initial_sequence_number{};
  /// @brief The console's first reliable sequence number. By default the
  /// console's tag is used, as chiaki-ng does, not the number its INIT_ACK
  /// advertises; see RemoteSequencePolicy.
  InitialSequenceNumber remote_initial_sequence_number{};
  /// @brief Outbound streams we asked for.
  StreamCount outbound_streams{};
  /// @brief Inbound streams we asked for.
  StreamCount inbound_streams{};
  /// @brief The console's receive window, which seeds the reliable channel's
  /// view of how much it may send.
  AdvertisedReceiveWindow remote_advertised_receive_window{};
};

}  // namespace libchiaking::net::protocol::takion
