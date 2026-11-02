/// @file
/// @brief Declares DatagramSecurity, a Kakusan spoke's pinned DTLS association.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/dtls/provider.hpp>

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Largest DER certificate accepted from the peer, in bytes.
inline constexpr std::size_t max_certificate_size = 4096;
/// @brief Records sent before the association must close: 2^24.
inline constexpr std::uint64_t default_record_limit = std::uint64_t{1} << 24U;
/// @brief Time allowed for the DTLS handshake, in microseconds: 15 seconds.
inline constexpr std::uint64_t security_handshake_timeout_us = 15'000'000;
/// @brief First DTLS handshake retransmission timeout, in microseconds: 400 ms.
inline constexpr std::uint64_t security_retransmit_initial_us = 400'000;
/// @brief Largest DTLS handshake retransmission timeout, in microseconds: 60
/// seconds.
inline constexpr std::uint64_t security_retransmit_max_us = 60'000'000;

/// @brief DTLS client (the host) or server (a guest).
using SecurityRole = platform::SecureDatagramRole;
/// @brief Where the association stands.
using SecurityState = platform::SecureDatagramState;

/// @brief How a spoke's DTLS association is opened.
struct DatagramSecurityConfig {
  /// @brief The ALPN to require.
  std::string alpn{application_protocol};
  /// @brief Records the association may send.
  std::uint64_t record_limit{default_record_limit};
  /// @brief Time allowed for the handshake, in microseconds.
  std::uint64_t handshake_timeout_us{security_handshake_timeout_us};
};

/// @brief One spoke's DTLS 1.2 association with certificate pinning, over the
/// platform's secure-datagram provider (RCP WIRE section 3).
///
/// Sans-I/O: received datagrams are fed in, and datagrams to send and records
/// received are taken out. MTU 1200, peer certificates up to 4096 bytes,
/// pinned by SHA-256. Owns the session; not copyable; one thread.
class DatagramSecurity {
 public:
  /// @brief Opens an association and makes this side's certificate.
  ///
  /// @param provider Makes the session.
  /// @param role Client or server.
  /// @param config ALPN, record limit and handshake timeout.
  /// @return The association, not yet started.
  /// @retval Error The provider refused; see protocol_error().
  [[nodiscard]] static auto open(platform::SecureDatagramProvider& provider,
                                 SecurityRole role,
                                 DatagramSecurityConfig config = {})
      -> Result<std::unique_ptr<DatagramSecurity>>;

  /// @brief Not copyable: it owns the session.
  DatagramSecurity(const DatagramSecurity&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  DatagramSecurity& operator=(const DatagramSecurity&) = delete;

  /// @brief SHA-256 of this side's DER certificate, published in its block.
  ///
  /// @return The digest.
  auto certificate_digest() const noexcept -> const Digest256&;
  /// @brief Starts the handshake, pinning the peer's certificate.
  ///
  /// @param peer_certificate_digest The digest from the peer's block.
  /// @param now_us The current monotonic time.
  /// @return Nothing on success.
  /// @retval Error The session refused.
  [[nodiscard]] auto start(const Digest256& peer_certificate_digest,
                           std::uint64_t now_us) -> Result<void>;
  /// @brief Feeds a datagram from the nominated pair.
  ///
  /// @param datagram The datagram.
  /// @param now_us The current monotonic time.
  /// @return Nothing on success; invalid ciphertext is discarded silently.
  /// @retval Error The handshake failed or the peer violated the protocol.
  [[nodiscard]] auto receive(std::span<const std::byte> datagram,
                             std::uint64_t now_us) -> Result<void>;
  /// @brief Runs retransmissions and timeouts.
  ///
  /// @param now_us The current monotonic time.
  /// @return Nothing on success.
  /// @retval Error::timeout The handshake took too long.
  [[nodiscard]] auto poll(std::uint64_t now_us) -> Result<void>;
  /// @brief Takes the next datagram to send.
  ///
  /// @return The datagram; nothing when none is waiting.
  [[nodiscard]] auto take_datagram() -> std::optional<std::vector<std::byte>>;
  /// @brief Takes the next application record received.
  ///
  /// @return The record; nothing when none is waiting.
  [[nodiscard]] auto take_record() -> std::optional<SecretBytes>;
  /// @brief Encrypts one application record.
  ///
  /// @param record The record, at most max_record_size().
  /// @return Nothing on success.
  /// @retval Error The association cannot send.
  [[nodiscard]] auto send(std::span<const std::byte> record) -> Result<void>;
  /// @brief Where the association stands.
  ///
  /// @return The state.
  auto state() const noexcept -> SecurityState;
  /// @brief Whether the handshake finished.
  ///
  /// @return True once established.
  auto established() const noexcept -> bool;
  /// @brief Largest application record that fits one datagram.
  ///
  /// @return The size in bytes.
  auto max_record_size() const noexcept -> std::size_t;
  /// @brief Exports keying material (RFC 5705) bound to this handshake.
  ///
  /// @param label The exporter label.
  /// @param context The context value.
  /// @param output Receives the material; its size is the length asked.
  /// @return Nothing on success.
  /// @retval Error The session cannot export.
  [[nodiscard]] auto export_secret(std::string_view label,
                                   std::span<const std::byte> context,
                                   std::span<std::byte> output) -> Result<void>;
  /// @brief When poll() must next run.
  ///
  /// @return The monotonic instant; nothing when no timer is armed.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Closes the association.
  void close() noexcept;

 private:
  /// @brief The platform session.
  using Session = platform::SecureDatagramSession;

  /// @brief Takes an opened session.
  ///
  /// @param session The session; not null.
  explicit DatagramSecurity(std::unique_ptr<Session> session) noexcept;

  /// @brief Translates a session failure.
  ///
  /// @param error The failure.
  /// @return timeout for a handshake timeout, protocol for peer-caused
  ///     failures, internal for local ones.
  static auto protocol_error(platform::SecureDatagramError error) noexcept
      -> Error;
  /// @brief Converts microseconds to the session's time type, saturating.
  ///
  /// @param now_us Microseconds.
  /// @return The value.
  static auto instant(std::uint64_t now_us) noexcept -> Session::Instant;
  /// @brief Translates a session result.
  ///
  /// @param result The result.
  /// @return Nothing, or the translated failure.
  static auto translate(
      const Expected<void, platform::SecureDatagramError>& result)
      -> Result<void>;

  /// @brief The session; never null.
  std::unique_ptr<Session> session_;
};

}  // namespace libchiaking::net::kakusan
