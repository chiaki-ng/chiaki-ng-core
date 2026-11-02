#include "src/core/net/protocol/remote_play/stream/active_handshake.hpp"

#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

auto ActiveHandshakeMessages::validate_big_inputs(
    const PreparedEcdh& prepared, const takion::WireProfile& profile)
    -> Expected<void, ActiveBigError> {
  const auto expected_curve = profile.ecdh_curve();
  if (platform::ecdh_shared_secret_size(expected_curve) == 0 ||
      prepared.curve != expected_curve)
    return unexpected(ActiveBigError::curve_mismatch);
  if (!prepared.key_pair ||
      prepared.public_key.size !=
          platform::ecdh_public_coordinates_size(expected_curve) + 1 ||
      prepared.public_key.bytes[0] != ec_point_uncompressed_tag)
    return unexpected(ActiveBigError::invalid_public_key);
  return {};
}

auto ActiveHandshakeMessages::make_big_base(const PreparedEcdh& prepared,
                                            const takion::WireProfile& profile)
    -> takion::Big {
  const auto wire = prepared.public_key.view();
  takion::Big big;
  big.client_version = profile.version();
  big.encrypted_key =
      std::vector<std::byte>(big_encrypted_key_size, std::byte{0});
  big.ecdh_pub_key = std::vector<std::byte>(wire.begin(), wire.end());
  big.ecdh_sig = std::vector<std::byte>(prepared.public_key.signature.begin(),
                                        prepared.public_key.signature.end());
  return big;
}

auto ActiveHandshakeMessages::make_big(
    const PreparedEcdh& prepared, const takion::WireProfile& profile,
    std::shared_ptr<const SecretString> session_key,
    std::shared_ptr<const SecretString> encrypted_launch_spec)
    -> Expected<takion::Big, ActiveBigError> {
  if (!session_key || session_key->empty())
    return unexpected(ActiveBigError::invalid_session_key);
  if (!encrypted_launch_spec || encrypted_launch_spec->empty())
    return unexpected(ActiveBigError::invalid_launch_spec);
  auto validated = validate_big_inputs(prepared, profile);
  if (!validated) return unexpected(validated.error());
  auto big = make_big_base(prepared, profile);
  big.session_key = takion::SharedSecretText{std::move(session_key)};
  big.launch_spec = takion::SharedSecretText{std::move(encrypted_launch_spec)};
  return big;
}

auto ActiveHandshakeMessages::make_crypto_install(
    takion::CryptoProvider& provider, const takion::WireProfile& profile,
    std::span<const std::byte> handshake_key, EcdhSharedSecret secret)
    -> Expected<takion::InstallCrypto, ActiveCryptoError> {
  if (handshake_key.size() != ecdh_handshake_key_size)
    return unexpected(ActiveCryptoError::invalid_handshake_key);

  const auto expected_secret_size =
      platform::ecdh_shared_secret_size(profile.ecdh_curve());
  if (expected_secret_size == 0)
    return unexpected(ActiveCryptoError::unsupported_curve);
  if (secret.size != expected_secret_size)
    return unexpected(ActiveCryptoError::invalid_secret);

  takion::CryptoMaterial local{takion::SecretBytes{handshake_key},
                               takion::SecretBytes{secret.view()},
                               crypto_index_local};
  takion::CryptoMaterial remote{takion::SecretBytes{handshake_key},
                                takion::SecretBytes{secret.view()},
                                crypto_index_remote};
  return takion::InstallCrypto{&provider, std::move(local), std::move(remote),
                               0, 0};
}

auto ActiveHandshakeMessages::map_ecdh_error(EcdhWireError error) noexcept
    -> ActiveBangError {
  using E = EcdhWireError;
  switch (error) {
    case E::invalid_handshake_key:
      return ActiveBangError::invalid_handshake_key;
    case E::invalid_wire_public_key:
      return ActiveBangError::invalid_remote_key;
    case E::invalid_remote_signature:
    case E::signature_mismatch:
      return ActiveBangError::invalid_remote_signature;
    default:
      return ActiveBangError::crypto_failure;
  }
}

auto ActiveHandshakeMessages::verify_bang(
    platform::CryptoProvider& crypto, const PreparedEcdh& prepared,
    const takion::WireProfile& profile,
    std::span<const std::byte> handshake_key,
    std::optional<std::string_view> required_session_key,
    const takion::Bang& bang) -> Expected<EcdhSharedSecret, ActiveBangError> {
  if (!bang.version_accepted)
    return unexpected(ActiveBangError::version_rejected);
  if (!bang.encrypted_key_accepted)
    return unexpected(ActiveBangError::encrypted_key_rejected);
  if (required_session_key && bang.session_key.view() != *required_session_key)
    return unexpected(ActiveBangError::session_key_mismatch);
  if (!bang.ecdh_pub_key || bang.ecdh_pub_key->empty() || !bang.ecdh_sig ||
      bang.ecdh_sig->empty())
    return unexpected(ActiveBangError::missing_remote_key);

  const auto curve = profile.ecdh_curve();
  if (platform::ecdh_shared_secret_size(curve) == 0 ||
      prepared.curve != curve || !prepared.key_pair)
    return unexpected(ActiveBangError::curve_mismatch);
  auto secret = EcdhWireCodec::decode_shared_secret(
      crypto, *prepared.key_pair, curve, handshake_key, *bang.ecdh_pub_key,
      *bang.ecdh_sig);
  if (!secret) return unexpected(map_ecdh_error(secret.error()));
  return std::move(secret).value();
}

ActiveHandshake::ActiveHandshake(
    PreparedEcdh prepared, takion::WireProfile profile,
    std::shared_ptr<const SecretString> session_key,
    std::shared_ptr<const SecretString> encrypted_launch_spec,
    BangSessionKeyPolicy session_key_policy,
    std::vector<std::uint32_t> negotiable_versions)
    : prepared_(std::move(prepared)),
      profile_(profile),
      negotiable_versions_(std::move(negotiable_versions)),
      session_key_(std::move(session_key)),
      encrypted_launch_spec_(std::move(encrypted_launch_spec)),
      session_key_policy_(session_key_policy),
      lifecycle_(std::make_unique<active_handshake_lifecycle::Lifecycle>()) {}

auto ActiveHandshake::sent_session_key() const noexcept -> std::string_view {
  return session_key_ ? session_key_->view() : std::string_view{};
}

void ActiveHandshake::on_session_key_mismatch_tolerated() noexcept {
  if (session_key_policy_ == BangSessionKeyPolicy::report_mismatch)
    session_key_mismatch_tolerated_ = true;
}

void ActiveHandshake::on_connected() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::TransportConnected{}));
}

auto ActiveHandshake::accepts_version(std::uint32_t version) const noexcept
    -> bool {
  return version == profile_.version() ||
         std::ranges::contains(negotiable_versions_, version);
}

auto ActiveHandshake::on_profile_committed(
    std::uint32_t version, std::optional<PreparedEcdh> replacement)
    -> Expected<std::optional<takion::SendControl>, ActiveHandshakeError> {
  if (!lifecycle_->process(active_handshake_lifecycle::ProfileArrived{}))
    return std::optional<takion::SendControl>{};
  const auto effect = lifecycle_->take_effect();
  if (!effect ||
      !std::holds_alternative<active_handshake_lifecycle::BuildBig>(*effect))
    return unexpected(ActiveHandshakeError::unexpected_profile);
  return commit_profile(version, std::move(replacement));
}

auto ActiveHandshake::commit_profile(std::uint32_t version,
                                     std::optional<PreparedEcdh> replacement)
    -> Expected<std::optional<takion::SendControl>, ActiveHandshakeError> {
  const auto committed = accepts_version(version)
                             ? takion::resolve_takion_wire_profile(version)
                             : std::optional<takion::WireProfile>{};
  if (!committed)
    return refuse(active_handshake_lifecycle::VersionUnsupported{});
  profile_ = *committed;
  if (replacement) prepared_ = std::move(*replacement);
  auto big = ActiveHandshakeMessages::make_big(
      prepared_, profile_, session_key_, std::move(encrypted_launch_spec_));
  if (!big) return refuse(active_handshake_lifecycle::BigUnbuildable{});
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::BigBuilt{}));
  return std::optional<takion::SendControl>{takion::SendControl{
      takion::ControlMessage{std::move(big.value())}, std::nullopt}};
}

auto ActiveHandshake::refuse(
    active_handshake_lifecycle::VersionUnsupported event)
    -> Unexpected<ActiveHandshakeError> {
  static_cast<void>(lifecycle_->process(event));
  return unexpected(ActiveHandshakeError::version_mismatch);
}

auto ActiveHandshake::refuse(active_handshake_lifecycle::BigUnbuildable event)
    -> Unexpected<ActiveHandshakeError> {
  static_cast<void>(lifecycle_->process(event));
  return unexpected(ActiveHandshakeError::big_failure);
}

auto ActiveHandshake::on_bang(takion::Bang bang)
    -> std::optional<takion::Bang> {
  if (!lifecycle_->process(active_handshake_lifecycle::BangArrived{}))
    return std::nullopt;
  return bang;
}

auto ActiveHandshake::on_stream_info(takion::StreamInfo info)
    -> std::optional<takion::StreamInfo> {
  if (!lifecycle_->process(
          active_handshake_lifecycle::StreamInfoArrived{info}) ||
      !lifecycle_->take_effect())
    return std::nullopt;
  return info;
}

auto ActiveHandshake::on_bang_verified(takion::CryptoProvider& provider,
                                       std::span<const std::byte> handshake_key,
                                       EcdhSharedSecret secret)
    -> Expected<takion::InstallCrypto, ActiveHandshakeError> {
  auto install = ActiveHandshakeMessages::make_crypto_install(
      provider, profile_, handshake_key, std::move(secret));
  if (!install) {
    if (!lifecycle_->process(active_handshake_lifecycle::CryptoUnpreparable{}))
      return unexpected(ActiveHandshakeError::unexpected_verification);
    return unexpected(ActiveHandshakeError::crypto_install_failure);
  }
  if (!lifecycle_->process(active_handshake_lifecycle::BangVerified{}))
    return unexpected(ActiveHandshakeError::unexpected_verification);
  return std::move(install.value());
}

void ActiveHandshake::on_bang_verification_failed() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::BangUnverifiable{}));
}

void ActiveHandshake::on_bang_rejected() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::BangRejected{}));
}

auto ActiveHandshake::on_crypto_installed()
    -> std::optional<takion::StreamInfo> {
  if (!lifecycle_->process(active_handshake_lifecycle::CryptoInstalled{}) ||
      !lifecycle_->take_effect())
    return std::nullopt;
  return lifecycle_->take_held_stream_info();
}

auto ActiveHandshake::on_stream_info_validated(ValidatedStreamInfo info)
    -> Expected<ReadyAction, ActiveHandshakeError> {
  if (!lifecycle_->process(active_handshake_lifecycle::StreamInfoAccepted{}))
    return unexpected(ActiveHandshakeError::unexpected_stream_info_validation);
  return ReadyAction{
      std::move(info),
      takion::SendControl{
          takion::ControlMessage{takion::StreamInfoAcknowledgement{}},
          std::nullopt}};
}

void ActiveHandshake::on_stream_info_validation_failed() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::StreamInfoInvalid{}));
}

void ActiveHandshake::on_stream_info_rejected() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::StreamInfoRejected{}));
}

void ActiveHandshake::on_stream_info_unacknowledged() noexcept {
  static_cast<void>(lifecycle_->process(
      active_handshake_lifecycle::StreamInfoUnacknowledged{}));
}

void ActiveHandshake::on_closed() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::SessionClosed{}));
}

void ActiveHandshake::on_terminal_failure() noexcept {
  static_cast<void>(
      lifecycle_->process(active_handshake_lifecycle::SessionFailed{}));
}

auto ActiveHandshake::authenticate_bang(
    ActiveHandshake& handshake, platform::CryptoProvider& crypto,
    takion::CryptoProvider& takion_crypto,
    std::span<const std::byte> handshake_key, takion::Bang bang)
    -> Expected<std::optional<takion::InstallCrypto>, AuthenticatedBangError> {
  auto accepted = handshake.on_bang(std::move(bang));
  if (!accepted) return std::optional<takion::InstallCrypto>{};

  const bool required =
      handshake.session_key_policy() == BangSessionKeyPolicy::required;
  auto secret = ActiveHandshakeMessages::verify_bang(
      crypto, handshake.prepared(), handshake.profile(), handshake_key,
      required ? std::optional<std::string_view>{handshake.sent_session_key()}
               : std::nullopt,
      *accepted);
  if (!secret) {
    handshake.on_bang_rejected();
    return unexpected(AuthenticatedBangError{secret.error()});
  }
  if (accepted->session_key.view() != handshake.sent_session_key())
    handshake.on_session_key_mismatch_tolerated();

  auto install = handshake.on_bang_verified(takion_crypto, handshake_key,
                                            std::move(secret.value()));
  if (!install) return unexpected(AuthenticatedBangError{install.error()});
  return std::optional<takion::InstallCrypto>{std::move(install.value())};
}

}  // namespace libchiaking::net::protocol::remote_play
