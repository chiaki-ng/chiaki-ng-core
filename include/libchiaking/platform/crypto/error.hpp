/// @file
/// @brief Declares the error a CryptoProvider reports.
#pragma once

#include <cstdint>

namespace libchiaking::platform {

/// @brief Reason a cryptographic primitive did not produce a result.
enum class CryptoError : std::uint8_t {
  /// @brief A key, input or output span has a size the primitive does not
  /// accept, or an encoded point or parameter is not valid.
  invalid_argument,
  /// @brief The underlying cryptography library reported a failure.
  backend_failure,
  /// @brief The provider does not implement this primitive.
  ///
  /// The library treats this as the feature being absent: for example shared
  /// play is refused when the identity primitives are missing.
  unsupported_capability,
};

}  // namespace libchiaking::platform
