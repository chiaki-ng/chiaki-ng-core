/// @file
/// @brief Declares the stock SecureDatagramProvider built on OpenSSL.
#pragma once

#include <libchiaking/platform/dtls/provider.hpp>

namespace libchiaking::platform::openssl {

/// @brief Stock SecureDatagramProvider that runs DTLS 1.2 with OpenSSL.
///
/// It depends on OpenSSL 3.2 or newer. Each session generates its own
/// self-signed P-256 certificate, accepts only the
/// ECDHE-ECDSA-AES128-GCM-SHA256 cipher suite on P-256, and authenticates the
/// peer solely by the pinned SHA-256 digest of its certificate. The provider
/// itself holds no state.
class OpenSslSecureDatagramProvider final : public SecureDatagramProvider {
 public:
  /// @brief Opens an OpenSSL-backed session. See
  /// SecureDatagramProvider::open.
  ///
  /// Blocks briefly while OpenSSL generates the key and certificate; nothing
  /// is sent. Safe to call from several threads at once.
  ///
  /// @param profile Role, protocol name, sizes, timers and record limit.
  ///     Copied.
  /// @return An idle session owned by the caller.
  /// @retval SecureDatagramError::invalid_profile A profile field is out of
  ///     range, for example an empty protocol name, an MTU beyond int or a
  ///     zero handshake deadline.
  /// @retval SecureDatagramError::unsupported OpenSSL refused DTLS 1.2, the
  ///     cipher suite, the P-256 group or the signature algorithms, or could
  ///     not create the P-256 key.
  /// @retval SecureDatagramError::internal OpenSSL failed to set up the
  ///     context or the certificate.
  [[nodiscard]] auto open(const SecureDatagramProfile& profile)
      -> Expected<std::unique_ptr<SecureDatagramSession>,
                  SecureDatagramError> override;
};

}  // namespace libchiaking::platform::openssl
