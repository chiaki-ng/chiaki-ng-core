/// @file
/// @brief Declares CHandle, the owning smart pointer for objects of C
/// libraries such as OpenSSL and Opus that are released by a free function.
#pragma once

#include <memory>

namespace libchiaking::memory {

/// @brief A stateless deleter that releases a C object with its library's
/// free function.
///
/// Any value the free function returns is discarded. Stateless, so a CHandle
/// is the size of a raw pointer.
///
/// @tparam Free The release function, such as `BN_clear_free`; must accept
///     the handle pointer and must not throw.
template <auto Free>
struct CDeleter {
  /// @brief Releases the object.
  ///
  /// @tparam Handle The C object type.
  /// @param handle The object to release; never null, since
  ///     `std::unique_ptr` does not call its deleter for null.
  template <class Handle>
  void operator()(Handle* handle) const noexcept {
    static_cast<void>(Free(handle));
  }
};

/// @brief Sole ownership of a C object, released with `Free` when the handle
/// is destroyed or reset.
///
/// @tparam Handle The C object type, such as `BIGNUM`.
/// @tparam Free The library function that releases it.
template <class Handle, auto Free>
using CHandle = std::unique_ptr<Handle, CDeleter<Free>>;

}  // namespace libchiaking::memory
