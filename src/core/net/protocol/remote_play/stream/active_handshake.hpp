/// @file
/// @brief Declares ActiveHandshake and ActiveHandshakeMessages, the client's
/// side of the stream's BIG/BANG key exchange and stream info.
///
/// After Takion connects and commits a version, the client sends BIG: its
/// Takion version, a 4-byte zero "encrypted key", its signed ECDH public key,
/// the session key (the control channel's session id) and the sealed launch
/// specification. The console answers BANG with its own signed key and
/// whether it accepted the version and key. Both ends then derive the stream
/// ciphers from the handshake key and the ECDH secret (key index 2 for the
/// client's direction, 3 for the console's), and the console sends the stream
/// info, which the client acknowledges.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/stream/active_handshake_lifecycle.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why BIG could not be built.
enum class ActiveBigError : std::uint8_t {
  /// @brief The session key is missing or empty.
  invalid_session_key,
  /// @brief The launch specification is missing or empty.
  invalid_launch_spec,
  /// @brief The ECDH key is not on the Takion version's curve.
  curve_mismatch,
  /// @brief The ECDH public key is malformed.
  invalid_public_key,
};

/// @brief Why the stream ciphers could not be prepared.
enum class ActiveCryptoError : std::uint8_t {
  /// @brief The handshake key is not 16 bytes.
  invalid_handshake_key,
  /// @brief The ECDH secret is not the curve's size.
  invalid_secret,
  /// @brief The curve is not known.
  unsupported_curve,
};

/// @brief Whether BANG must repeat the session key the client sent.
enum class BangSessionKeyPolicy : std::uint8_t {
  /// @brief It must; a mismatch refuses BANG.
  required,
  /// @brief It need not (the id was synthesised); a mismatch is only
  /// recorded.
  report_mismatch,
};

/// @brief Why BANG was refused.
enum class ActiveBangError : std::uint8_t {
  /// @brief The console refused the client's version.
  version_rejected,
  /// @brief The console refused the client's encrypted key.
  encrypted_key_rejected,
  /// @brief BANG's session key differs from the one sent.
  session_key_mismatch,
  /// @brief BANG has no public key or signature.
  missing_remote_key,
  /// @brief The client's key is not on the version's curve.
  curve_mismatch,
  /// @brief The handshake key is not 16 bytes.
  invalid_handshake_key,
  /// @brief The console's public key is malformed.
  invalid_remote_key,
  /// @brief The console's signature is missing, short or wrong.
  invalid_remote_signature,
  /// @brief Agreeing the secret failed.
  crypto_failure,
};

/// @brief Builds BIG, checks BANG and prepares the stream ciphers.
///
/// Stateless; every member is static.
class ActiveHandshakeMessages {
 public:
  /// @brief The size of BIG's encrypted key, sent as zeros.
  static constexpr std::size_t big_encrypted_key_size = 4;
  /// @brief The key index of the client-to-console cipher.
  static constexpr std::uint8_t crypto_index_local = 2;
  /// @brief The key index of the console-to-client cipher.
  static constexpr std::uint8_t crypto_index_remote = 3;

  /// @brief Makes the cipher installation for both directions.
  ///
  /// @param provider The Takion crypto provider.
  /// @param profile The committed wire profile.
  /// @param handshake_key The 16-byte handshake key.
  /// @param secret The agreed ECDH secret.
  /// @return The installation, both key positions starting at 0.
  /// @retval ActiveCryptoError Why it could not be made.
  [[nodiscard]] static auto make_crypto_install(
      takion::CryptoProvider& provider, const takion::WireProfile& profile,
      std::span<const std::byte> handshake_key, EcdhSharedSecret secret)
      -> Expected<takion::InstallCrypto, ActiveCryptoError>;

  /// @brief Builds BIG.
  ///
  /// @param prepared The client's ECDH key.
  /// @param profile The committed wire profile.
  /// @param session_key The session id.
  /// @param encrypted_launch_spec The sealed launch specification.
  /// @return The message.
  /// @retval ActiveBigError Why it could not be built.
  [[nodiscard]] static auto make_big(
      const PreparedEcdh& prepared, const takion::WireProfile& profile,
      std::shared_ptr<const SecretString> session_key,
      std::shared_ptr<const SecretString> encrypted_launch_spec)
      -> Expected<takion::Big, ActiveBigError>;

  /// @brief Checks BANG and agrees the ECDH secret.
  ///
  /// @param crypto The platform crypto provider.
  /// @param prepared The client's ECDH key.
  /// @param profile The committed wire profile.
  /// @param handshake_key The 16-byte handshake key.
  /// @param required_session_key The session key BANG must repeat, or nothing
  ///     when any will do.
  /// @param bang The console's message.
  /// @return The secret.
  /// @retval ActiveBangError Why BANG was refused.
  [[nodiscard]] static auto verify_bang(
      platform::CryptoProvider& crypto, const PreparedEcdh& prepared,
      const takion::WireProfile& profile,
      std::span<const std::byte> handshake_key,
      std::optional<std::string_view> required_session_key,
      const takion::Bang& bang) -> Expected<EcdhSharedSecret, ActiveBangError>;

 private:
  /// @brief Checks the ECDH key against the profile's curve.
  ///
  /// @param prepared The client's ECDH key.
  /// @param profile The committed wire profile.
  /// @return Nothing, or the problem.
  [[nodiscard]] static auto validate_big_inputs(
      const PreparedEcdh& prepared, const takion::WireProfile& profile)
      -> Expected<void, ActiveBigError>;
  /// @brief BIG without its session key and launch specification.
  ///
  /// @param prepared The client's ECDH key.
  /// @param profile The committed wire profile.
  /// @return The message.
  static auto make_big_base(const PreparedEcdh& prepared,
                            const takion::WireProfile& profile) -> takion::Big;
  /// @brief Maps an ECDH failure to a BANG refusal.
  ///
  /// @param error The ECDH failure.
  /// @return The refusal.
  static auto map_ecdh_error(EcdhWireError error) noexcept -> ActiveBangError;
};

/// @brief Why a handshake step failed.
enum class ActiveHandshakeError : std::uint8_t {
  /// @brief The version arrived in a phase that cannot take it.
  unexpected_profile,
  /// @brief The committed version is not one this session accepts.
  version_mismatch,
  /// @brief BIG could not be built.
  big_failure,
  /// @brief BANG was verified in a phase that cannot take it.
  unexpected_verification,
  /// @brief The ciphers could not be prepared.
  crypto_install_failure,
  /// @brief The stream info was accepted in a phase that cannot take it.
  unexpected_stream_info_validation,
};

/// @brief What to do once the stream info is accepted.
struct ReadyAction {
  /// @brief The checked stream info.
  ValidatedStreamInfo info;
  /// @brief The acknowledgement to send.
  takion::SendControl ack;
};

/// @brief A refused or failed BANG.
struct AuthenticatedBangError;

/// @brief One active session's handshake.
///
/// Owns the ECDH key and keeps the session key and launch specification
/// until BIG is built. Not thread-safe; the session's worker drives it.
class ActiveHandshake {
 public:
  /// @brief Checks BANG and, if it passes, prepares the ciphers.
  ///
  /// A BANG that arrives in the wrong phase is ignored.
  ///
  /// @param handshake The handshake.
  /// @param crypto The platform crypto provider.
  /// @param takion_crypto The Takion crypto provider.
  /// @param handshake_key The 16-byte handshake key.
  /// @param bang The console's message.
  /// @return The installation, or nothing when BANG was ignored.
  /// @retval AuthenticatedBangError Why BANG was refused or the ciphers
  ///     failed.
  [[nodiscard]] static auto authenticate_bang(
      ActiveHandshake& handshake, platform::CryptoProvider& crypto,
      takion::CryptoProvider& takion_crypto,
      std::span<const std::byte> handshake_key, takion::Bang bang)
      -> Expected<std::optional<takion::InstallCrypto>, AuthenticatedBangError>;

  /// @brief Makes a handshake waiting for the connection.
  ///
  /// @param prepared The client's ECDH key.
  /// @param profile The wire profile it expects.
  /// @param session_key The session id.
  /// @param encrypted_launch_spec The sealed launch specification.
  /// @param session_key_policy Whether BANG must repeat the session key.
  /// @param negotiable_versions Other versions the session accepts.
  ActiveHandshake(
      PreparedEcdh prepared, takion::WireProfile profile,
      std::shared_ptr<const SecretString> session_key,
      std::shared_ptr<const SecretString> encrypted_launch_spec,
      BangSessionKeyPolicy session_key_policy = BangSessionKeyPolicy::required,
      std::vector<std::uint32_t> negotiable_versions = {});

  /// @brief The phase.
  ///
  /// @return The state.
  auto state() const noexcept -> ActiveHandshakeState {
    return lifecycle_->state();
  }
  /// @brief The client's ECDH key.
  ///
  /// @return The key.
  auto prepared() const noexcept -> const PreparedEcdh& { return prepared_; }
  /// @brief The wire profile, committed once the version is.
  ///
  /// @return The profile.
  auto profile() const noexcept -> const takion::WireProfile& {
    return profile_;
  }
  /// @brief The session key sent in BIG.
  ///
  /// @return The key, or empty.
  auto sent_session_key() const noexcept -> std::string_view;
  /// @brief Whether BANG must repeat the session key.
  ///
  /// @return The policy.
  auto session_key_policy() const noexcept -> BangSessionKeyPolicy {
    return session_key_policy_;
  }
  /// @brief Whether BANG's session key differed and was let through.
  ///
  /// @return True when it was.
  auto session_key_mismatch_tolerated() const noexcept -> bool {
    return session_key_mismatch_tolerated_;
  }
  /// @brief Records that BANG's session key differed, when the policy allows.
  void on_session_key_mismatch_tolerated() noexcept;

  /// @brief Takion connected.
  void on_connected() noexcept;
  /// @brief Whether the session accepts a version.
  ///
  /// @param version The version.
  /// @return True for the expected or a negotiable version.
  auto accepts_version(std::uint32_t version) const noexcept -> bool;
  /// @brief Takion committed a version: builds BIG.
  ///
  /// @param version The committed version.
  /// @param replacement An ECDH key for the version's curve, when it differs.
  /// @return BIG to send, or nothing when the version came in the wrong phase.
  /// @retval ActiveHandshakeError::unexpected_profile No BIG was due.
  /// @retval ActiveHandshakeError::version_mismatch The version is refused.
  /// @retval ActiveHandshakeError::big_failure BIG could not be built.
  [[nodiscard]] auto on_profile_committed(
      std::uint32_t version,
      std::optional<PreparedEcdh> replacement = std::nullopt)
      -> Expected<std::optional<takion::SendControl>, ActiveHandshakeError>;
  /// @brief BANG arrived.
  ///
  /// @param bang The message.
  /// @return It, or nothing when it came in the wrong phase.
  [[nodiscard]] auto on_bang(takion::Bang bang) -> std::optional<takion::Bang>;
  /// @brief The stream info arrived.
  ///
  /// @param info The message.
  /// @return It when it should be checked now, or nothing when it was held
  ///     for after the ciphers or ignored.
  [[nodiscard]] auto on_stream_info(takion::StreamInfo info)
      -> std::optional<takion::StreamInfo>;
  /// @brief BANG was verified: prepares the ciphers.
  ///
  /// @param provider The Takion crypto provider.
  /// @param handshake_key The 16-byte handshake key.
  /// @param secret The agreed secret.
  /// @return The installation.
  /// @retval ActiveHandshakeError::crypto_install_failure The ciphers failed.
  /// @retval ActiveHandshakeError::unexpected_verification Wrong phase.
  [[nodiscard]] auto on_bang_verified(takion::CryptoProvider& provider,
                                      std::span<const std::byte> handshake_key,
                                      EcdhSharedSecret secret)
      -> Expected<takion::InstallCrypto, ActiveHandshakeError>;
  /// @brief BANG could not be verified; the handshake fails.
  void on_bang_verification_failed() noexcept;
  /// @brief BANG was refused; the handshake waits for another.
  void on_bang_rejected() noexcept;
  /// @brief The ciphers are installed.
  ///
  /// @return A stream info held from before, to check now, or nothing.
  [[nodiscard]] auto on_crypto_installed() -> std::optional<takion::StreamInfo>;
  /// @brief The stream info passed its checks.
  ///
  /// @param info The checked stream info.
  /// @return The acknowledgement to send.
  /// @retval ActiveHandshakeError::unexpected_stream_info_validation Wrong
  ///     phase.
  [[nodiscard]] auto on_stream_info_validated(ValidatedStreamInfo info)
      -> Expected<ReadyAction, ActiveHandshakeError>;
  /// @brief The stream info is unusable; the handshake fails.
  void on_stream_info_validation_failed() noexcept;
  /// @brief The stream info was refused; the handshake waits for another.
  void on_stream_info_rejected() noexcept;
  /// @brief The acknowledgement was not taken; the handshake waits for the
  /// stream info again.
  void on_stream_info_unacknowledged() noexcept;
  /// @brief The session closed.
  void on_closed() noexcept;
  /// @brief The session failed.
  void on_terminal_failure() noexcept;

 private:
  /// @brief Commits the version and builds BIG.
  ///
  /// @param version The version.
  /// @param replacement A replacement ECDH key.
  /// @return As on_profile_committed().
  [[nodiscard]] auto commit_profile(std::uint32_t version,
                                    std::optional<PreparedEcdh> replacement)
      -> Expected<std::optional<takion::SendControl>, ActiveHandshakeError>;
  /// @brief Fails on a refused version.
  ///
  /// @param event The event.
  /// @return ActiveHandshakeError::version_mismatch.
  [[nodiscard]] auto refuse(
      active_handshake_lifecycle::VersionUnsupported event)
      -> Unexpected<ActiveHandshakeError>;
  /// @brief Fails on an unbuildable BIG.
  ///
  /// @param event The event.
  /// @return ActiveHandshakeError::big_failure.
  [[nodiscard]] auto refuse(active_handshake_lifecycle::BigUnbuildable event)
      -> Unexpected<ActiveHandshakeError>;

  /// @brief The client's ECDH key.
  PreparedEcdh prepared_;
  /// @brief The wire profile.
  takion::WireProfile profile_;
  /// @brief Other versions the session accepts.
  std::vector<std::uint32_t> negotiable_versions_;
  /// @brief The session id sent in BIG.
  std::shared_ptr<const SecretString> session_key_;
  /// @brief The launch specification until BIG takes it.
  std::shared_ptr<const SecretString> encrypted_launch_spec_;
  /// @brief Whether BANG must repeat the session key.
  BangSessionKeyPolicy session_key_policy_{BangSessionKeyPolicy::required};
  /// @brief Whether a mismatched session key was let through.
  bool session_key_mismatch_tolerated_{false};
  /// @brief The state machine.
  std::unique_ptr<active_handshake_lifecycle::Lifecycle> lifecycle_;
};

/// @brief Why BANG was refused or the ciphers failed.
struct AuthenticatedBangError {
  /// @brief The BANG refusal or the handshake failure.
  std::variant<ActiveBangError, ActiveHandshakeError> reason;
};

}  // namespace libchiaking::net::protocol::remote_play
