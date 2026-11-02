/// @file
/// @brief Declares AbsentSecureDatagramProvider, the stand-in a factory
/// returns when the host supplies no DTLS port.
#pragma once

#include <libchiaking/platform/dtls/provider.hpp>

namespace libchiaking::platform {

/// @brief SecureDatagramProvider that cannot open any session.
///
/// A factory returns it from PlatformFactory::secure_datagram when the host
/// supplies no provider, and leaves PlatformCapability::secure_datagram unset.
/// It has no state and may be called from any thread at once.
class AbsentSecureDatagramProvider final : public SecureDatagramProvider {
 public:
  /// @brief Refuses to open a session.
  ///
  /// @param profile Ignored.
  /// @return Always SecureDatagramError::unsupported.
  [[nodiscard]] auto open(const SecureDatagramProfile& profile)
      -> Expected<std::unique_ptr<SecureDatagramSession>,
                  SecureDatagramError> override {
    (void)profile;
    return unexpected(SecureDatagramError::unsupported);
  }
};

}  // namespace libchiaking::platform
