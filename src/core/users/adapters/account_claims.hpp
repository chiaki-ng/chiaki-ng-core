/// @file
/// @brief Declares AccountClaims, how the account facts inside a PSN token's
/// claims are read.
#pragma once

#include "libchiaking/detail/users/account_context.hpp"

#include <cstddef>
#include <string_view>

namespace libchiaking::users {

/// @brief Reads an AccountContext from the claims of a JWT-shaped PSN token.
/// Holds no state; every member is static.
class AccountClaims {
 public:
  /// @brief Longest token read() will read, in bytes.
  static constexpr std::size_t token_limit = 8192;

  /// @brief Reads the context from the claims of a JWT-shaped token.
  ///
  /// The token's middle segment is decoded and its `legal_country`, `locale`,
  /// `age`, `is_child` and `iat` claims are taken, then validated as
  /// AccountContext::stating does. The signature is not checked; the caller
  /// is expected to pass a token it received from PSN. The decoded claims are
  /// held in wiping memory while they are read.
  ///
  /// @param token The token text.
  /// @return The context. It states nothing when the token is longer than
  ///     token_limit, is not three dot-separated segments, or its payload is
  ///     not a JSON object.
  static auto read(std::string_view token) -> AccountContext;
};

}  // namespace libchiaking::users
