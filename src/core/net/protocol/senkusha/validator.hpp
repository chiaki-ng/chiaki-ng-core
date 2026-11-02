/// @file
/// @brief Declares Validator, the rules a connection test's
/// configuration and the console's BANG must satisfy.
#pragma once

#include "src/core/net/protocol/senkusha/types.hpp"

#include <cstdint>

namespace libchiaking::net::protocol::senkusha {

/// @brief Checks a Senkusha profile and policy before a test runs, and the
/// BANG that answers its BIG.
///
/// Stateless; every member is a static function safe to call from any
/// thread.
class Validator {
 public:
  /// @brief Checks that a test can run as configured.
  ///
  /// A CloudStream echo profile must pin version 9 for bootstrap, fixed
  /// selection and the BIG's client version, and its BIG must carry a
  /// session key and nothing else. Every version must be implemented and
  /// available to console sessions, with no repeats. Timeouts must be
  /// positive, the probe count 1 to 2048, the MTU bounds ascending with the
  /// lower one large enough for a minimal probe, and the probe tag must end
  /// within the minimum payload.
  ///
  /// @param profile What the far end expects.
  /// @param policy Timeouts and measurement settings.
  /// @return Nothing when the test can run.
  /// @retval Error::invalid_configuration A rule above is broken,
  ///     the BIG's client version is 0, or it carries only one of the ECDH
  ///     key and signature.
  /// @retval Error::unsupported_big_response The response policy
  ///     is unknown.
  /// @retval Error::unsupported_version A version is not available.
  /// @retval Error::duplicate_version An offered version repeats.
  /// @retval Error::arithmetic_overflow A size sum does not fit 32
  ///     bits.
  [[nodiscard]] static auto validate_configuration(const Profile& profile,
                                                   const Policy& policy)
      -> Expected<void, Error>;

  /// @brief Checks a console's BANG against the profile's requirements.
  ///
  /// @param profile Supplies the BangValidationPolicy.
  /// @param bang The BANG received.
  /// @param committed_version The version the session committed.
  /// @return Nothing when the test may go on.
  /// @retval Error::unsupported_version The committed version is
  ///     not available.
  /// @retval Error::negotiation_failed A required acceptance flag
  ///     is clear.
  /// @retval Error::malformed_response A required protocol
  ///     acknowledgement is missing.
  [[nodiscard]] static auto validate_bang(const Profile& profile,
                                          const takion::Bang& bang,
                                          std::uint32_t committed_version)
      -> Expected<void, Error>;

 private:
  /// @brief Tells whether a duration is above zero.
  ///
  /// @tparam Duration A std::chrono::duration.
  /// @param value The duration.
  /// @return Whether its count is positive.
  template <class Duration>
  static bool positive(Duration value) noexcept;
  /// @brief Tells whether a version may be used for a console session.
  ///
  /// @param version The Takion version.
  /// @return Whether it is non-zero, implemented and not CloudStream only.
  static bool valid_version(std::uint32_t version) noexcept;
};

}  // namespace libchiaking::net::protocol::senkusha
