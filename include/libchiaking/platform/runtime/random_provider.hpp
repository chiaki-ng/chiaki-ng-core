/// @file
/// @brief Declares SecureRandomProvider, the source of unpredictable bytes,
/// and SystemSecureRandom, the stock one.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/error.hpp>

#include <span>

namespace libchiaking::platform {

/// @brief A cryptographically secure random number source.
///
/// Implemented by the host frontend, or by SystemSecureRandom. The library
/// reaches it through PlatformFactory::random and borrows it for as long as
/// anything built from that factory exists. The bytes feed nonces, keys and
/// identifiers, so they must come from a generator suitable for cryptography.
/// @ingroup api_platform
class SecureRandomProvider {
 public:
  /// @brief Destroys the source. The library never deletes a source it was
  /// given.
  virtual ~SecureRandomProvider() = default;
  /// @brief Overwrites a buffer with random bytes.
  ///
  /// @param output Buffer to fill completely. May be empty.
  /// @return Nothing on success; every byte of @p output has been replaced.
  /// @retval PlatformError::io_failure The entropy source failed.
  /// @retval PlatformError::out_of_memory The entropy source ran out of
  ///     memory.
  [[nodiscard]] virtual auto fill_bytes(std::span<std::byte> output)
      -> Expected<void, PlatformError> = 0;
};

/// @brief Stock SecureRandomProvider backed by the operating system's
/// generator.
///
/// It uses getentropy on POSIX systems and BCryptGenRandom on Windows, holds no
/// state, and may be called from any thread. DefaultPlatformFactory uses it
/// unless the host supplies another source.
class SystemSecureRandom final : public SecureRandomProvider {
 public:
  /// @brief Fills a buffer from the operating system's generator. See
  /// SecureRandomProvider::fill_bytes.
  ///
  /// @param output Buffer to fill. Any length is accepted; large buffers are
  ///     filled in several requests.
  /// @return Nothing on success.
  /// @retval PlatformError::io_failure The system call failed. Bytes written
  ///     before the failure are left in place.
  /// @retval PlatformError::out_of_memory The system reported that it was out
  ///     of memory.
  [[nodiscard]] auto fill_bytes(std::span<std::byte> output)
      -> Expected<void, PlatformError> override;
};

}  // namespace libchiaking::platform
