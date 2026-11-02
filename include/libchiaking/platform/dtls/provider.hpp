/// @file
/// @brief Declares the secure datagram port: a DTLS session the library drives
/// without giving it a socket.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {

/// @brief Size in bytes of a certificate digest used to pin a peer.
inline constexpr std::size_t secure_datagram_digest_size = 32;
/// @brief Longest application protocol name, in bytes, a SecureDatagramProfile
/// may carry.
inline constexpr std::size_t max_secure_datagram_alpn_size = 255;

/// @brief Which side of the handshake a session plays.
enum class SecureDatagramRole : std::uint8_t {
  /// @brief Sends the first handshake message.
  client,
  /// @brief Waits for the peer's first handshake message.
  server,
};

/// @brief Parameters the library fixes when it opens a secure datagram session.
///
/// The stock provider rejects a profile with an empty or over-long protocol
/// name, a zero MTU, certificate size or record limit, a non-positive deadline
/// or first retransmit delay, or a longest retransmit delay shorter than the
/// first.
struct SecureDatagramProfile {
  /// @brief Side of the handshake to play.
  SecureDatagramRole role{SecureDatagramRole::client};
  /// @brief Application protocol name both peers must negotiate; the
  /// handshake fails if they differ.
  std::string alpn;
  /// @brief Largest datagram, in bytes, the session may emit or accept.
  std::size_t mtu{};
  /// @brief Largest encoded certificate, in bytes, accepted from the peer.
  std::size_t max_certificate_size{};
  /// @brief Time allowed for the handshake, measured from
  /// SecureDatagramSession::start.
  std::chrono::microseconds handshake_deadline{};
  /// @brief Delay before the first retransmission of an unanswered handshake
  /// flight.
  std::chrono::microseconds retransmit_first{};
  /// @brief Ceiling the retransmission delay doubles up to.
  std::chrono::microseconds retransmit_max{};
  /// @brief Number of application records allowed in each direction before
  /// the session fails with SecureDatagramError::record_limit.
  std::uint64_t record_limit{};
};

/// @brief Reason a secure datagram operation failed.
enum class SecureDatagramError : std::uint8_t {
  /// @brief The backend cannot provide the required protocol version, cipher
  /// suite or key type.
  unsupported,
  /// @brief SecureDatagramProvider::open was given a profile with an
  /// out-of-range field.
  invalid_profile,
  /// @brief The call is not valid in the session's current
  /// SecureDatagramState.
  invalid_state,
  /// @brief An argument is empty, zero or larger than the session allows.
  invalid_argument,
  /// @brief The peer presented a certificate whose digest is not the pinned
  /// one.
  peer_not_pinned,
  /// @brief The handshake ended without agreeing on the required parameters.
  handshake_failed,
  /// @brief The handshake did not finish within
  /// SecureDatagramProfile::handshake_deadline.
  handshake_timeout,
  /// @brief SecureDatagramProfile::record_limit was reached in one direction.
  record_limit,
  /// @brief The peer sent data that breaks the protocol once the session was
  /// running.
  peer_violation,
  /// @brief The backend failed for a reason unrelated to the peer.
  internal,
};

/// @brief Lifecycle stage of a SecureDatagramSession.
enum class SecureDatagramState : std::uint8_t {
  /// @brief Created; SecureDatagramSession::start has not been called.
  idle,
  /// @brief The handshake is in progress.
  handshaking,
  /// @brief The handshake succeeded and the peer matched its pin; records can
  /// flow.
  established,
  /// @brief Shut down by SecureDatagramSession::close or by the peer.
  closed,
  /// @brief Ended by an error. Further calls that return an error repeat it.
  failed,
};

/// @brief One DTLS association with a single peer, driven entirely by its
/// caller.
///
/// Implemented by a SecureDatagramProvider. The session owns no socket and no
/// timer. The library pushes received datagrams in with receive(), tells it the
/// time with poll(), sends whatever take_datagram() yields, and reads decrypted
/// application data with take_record(). All times are readings of one monotonic
/// clock chosen by the caller, in microseconds.
///
/// The session generates its own certificate. Peers do not use a certificate
/// authority: each learns the other's certificate_digest() over a separate
/// channel and passes it to start(), and the handshake succeeds only against
/// that exact certificate.
///
/// The library owns the session through the pointer
/// SecureDatagramProvider::open returns and destroys it when the association
/// ends.
class SecureDatagramSession {
 public:
  /// @brief SHA-256 digest of an encoded certificate, used as a pin.
  using Digest = std::array<std::byte, secure_datagram_digest_size>;
  /// @brief A point on the caller's monotonic clock, in microseconds since
  /// that clock's origin.
  using Instant = std::chrono::microseconds;

  /// @brief Destroys the session and its key material.
  virtual ~SecureDatagramSession() = default;
  /// @brief Constructs the base of a provider's session type.
  SecureDatagramSession() = default;
  /// @brief Not copyable; a session holds one association's keys.
  SecureDatagramSession(const SecureDatagramSession&) = delete;
  /// @brief Not copy-assignable; a session holds one association's keys.
  auto operator=(const SecureDatagramSession&)
      -> SecureDatagramSession& = delete;

  /// @brief The pin for this session's own certificate.
  ///
  /// @return The digest the peer must pass to its own start(). Available as
  ///     soon as the session is opened and stable for its lifetime.
  virtual auto certificate_digest() const noexcept -> const Digest& = 0;
  /// @brief Begins the handshake against a pinned peer.
  ///
  /// @param pinned_peer_digest Digest of the only certificate the peer may
  ///     present.
  /// @param now Current time; the handshake deadline is counted from it.
  /// @return Nothing on success. A client's first flight is then waiting in
  ///     take_datagram().
  /// @retval SecureDatagramError::invalid_state The session is not
  ///     SecureDatagramState::idle.
  /// @retval SecureDatagramError::internal The backend failed.
  /// @see poll(), receive()
  [[nodiscard]] virtual auto start(const Digest& pinned_peer_digest,
                                   Instant now)
      -> Expected<void, SecureDatagramError> = 0;
  /// @brief Feeds one datagram received from the peer into the session.
  ///
  /// Decrypted application data becomes available from take_record() and any
  /// reply from take_datagram(). A session that is neither handshaking nor
  /// established ignores the call. The stock provider also ignores an empty
  /// datagram or one larger than the MTU without failing.
  ///
  /// @param datagram The datagram exactly as it arrived. It is not retained.
  /// @param now Current time.
  /// @return Nothing on success, including when the datagram was ignored.
  /// @retval SecureDatagramError::handshake_timeout The handshake deadline
  ///     has passed.
  /// @retval SecureDatagramError::peer_not_pinned The peer's certificate does
  ///     not match the pin.
  /// @retval SecureDatagramError::handshake_failed The handshake could not be
  ///     completed.
  /// @retval SecureDatagramError::record_limit The inbound record limit was
  ///     reached.
  /// @retval SecureDatagramError::peer_violation The peer broke the protocol.
  /// @retval SecureDatagramError::internal The backend failed.
  [[nodiscard]] virtual auto receive(std::span<const std::byte> datagram,
                                     Instant now)
      -> Expected<void, SecureDatagramError> = 0;
  /// @brief Advances the session's timers.
  ///
  /// Call it when next_deadline() is reached. It queues handshake
  /// retransmissions that have fallen due and fails the session once the
  /// handshake deadline has passed.
  ///
  /// @param now Current time.
  /// @return Nothing on success.
  /// @retval SecureDatagramError::handshake_timeout The handshake deadline
  ///     has passed.
  /// @retval SecureDatagramError::internal The backend failed.
  [[nodiscard]] virtual auto poll(Instant now)
      -> Expected<void, SecureDatagramError> = 0;
  /// @brief Removes the next datagram the session wants sent to the peer.
  ///
  /// Drain it after every start(), receive(), poll(), send() and close().
  ///
  /// @return One datagram to transmit unchanged, owned by the caller, or
  ///     std::nullopt when nothing is waiting.
  virtual auto take_datagram() -> std::optional<std::vector<std::byte>> = 0;
  /// @brief Removes the next decrypted application record received from the
  /// peer.
  ///
  /// @return One record in arrival order, held in memory that is wiped on
  ///     destruction, or std::nullopt when nothing is waiting.
  virtual auto take_record() -> std::optional<SecretBytes> = 0;
  /// @brief Encrypts one application record for the peer.
  ///
  /// The protected datagram is queued for take_datagram(); nothing is
  /// transmitted by this call.
  ///
  /// @param record Plaintext of 1 to max_record_size() bytes. It is not
  ///     retained.
  /// @return Nothing on success.
  /// @retval SecureDatagramError::invalid_state The session is not
  ///     SecureDatagramState::established.
  /// @retval SecureDatagramError::invalid_argument @p record is empty or too
  ///     large.
  /// @retval SecureDatagramError::record_limit The outbound record limit was
  ///     reached; the session has failed.
  /// @retval SecureDatagramError::peer_violation The backend refused the
  ///     write because of the peer's earlier behaviour; the session has
  ///     failed.
  /// @retval SecureDatagramError::internal The backend failed; the session
  ///     has failed.
  [[nodiscard]] virtual auto send(std::span<const std::byte> record)
      -> Expected<void, SecureDatagramError> = 0;
  /// @brief Derives key material bound to this session's handshake, for use
  /// outside the record layer.
  ///
  /// Both peers obtain the same bytes when they pass the same label, context
  /// and size.
  ///
  /// @param label Purpose string that separates one use from another.
  /// @param context Additional bytes mixed into the derivation. May be empty.
  /// @param size Number of bytes wanted. Must not be zero.
  /// @return The derived secret in memory that is wiped on destruction.
  /// @retval SecureDatagramError::invalid_state The session is not
  ///     SecureDatagramState::established.
  /// @retval SecureDatagramError::invalid_argument @p size is zero.
  /// @retval SecureDatagramError::internal The backend could not derive or
  ///     hold the secret.
  [[nodiscard]] virtual auto export_secret(std::string_view label,
                                           std::span<const std::byte> context,
                                           std::size_t size)
      -> Expected<SecretBytes, SecureDatagramError> = 0;
  /// @brief The session's current lifecycle stage.
  ///
  /// @return The stage reached by the last call on this session. It changes
  ///     only inside calls the owner makes, so it needs no locking when read
  ///     on the owner's thread.
  virtual auto state() const noexcept -> SecureDatagramState = 0;
  /// @brief Largest plaintext send() accepts.
  ///
  /// @return The limit in bytes while the session is established, otherwise
  ///     0.
  virtual auto max_record_size() const noexcept -> std::size_t = 0;
  /// @brief When poll() next needs to be called.
  ///
  /// @return The earliest pending retransmission or handshake deadline, or
  ///     std::nullopt when no timer is running.
  virtual auto next_deadline() const noexcept -> std::optional<Instant> = 0;
  /// @brief Ends the session.
  ///
  /// An established session queues a closing notice for take_datagram().
  /// Undelivered inbound records are discarded. The state becomes
  /// SecureDatagramState::closed unless the session had already failed.
  virtual void close() noexcept = 0;
};

/// @brief Factory for secure datagram sessions.
///
/// Implemented by the host frontend or by
/// openssl::OpenSslSecureDatagramProvider. The library reaches it through
/// PlatformFactory::secure_datagram, borrows it for as long as anything built
/// from that factory exists and opens sessions from it as it needs them. Shared
/// play is unavailable when the factory has no such provider.
class SecureDatagramProvider {
 public:
  /// @brief Destroys the provider. The library never deletes a provider it
  /// was given.
  virtual ~SecureDatagramProvider() = default;

  /// @brief Creates an idle session with a fresh certificate.
  ///
  /// @param profile Role, protocol name, sizes, timers and record limit for
  ///     the session. It is copied.
  /// @return The session, owned by the caller. It must not be null.
  /// @retval SecureDatagramError::invalid_profile A profile field is out of
  ///     range.
  /// @retval SecureDatagramError::unsupported The backend cannot provide the
  ///     required protocol parameters.
  /// @retval SecureDatagramError::internal The backend failed to set the
  ///     session up.
  [[nodiscard]] virtual auto open(const SecureDatagramProfile& profile)
      -> Expected<std::unique_ptr<SecureDatagramSession>,
                  SecureDatagramError> = 0;
};

}  // namespace libchiaking::platform
