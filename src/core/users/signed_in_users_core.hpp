/// @file
/// @brief Defines SignedInUsers::Core, the registry's internal state behind
/// the public SignedInUsers facade.
///
/// The public class only forwards; this file holds what the registry is made
/// of: the random source it falls back on when the application supplies
/// none, the shared PS clock, the roster of users and the sign-in use cases
/// that fill it.
#pragma once

#include "libchiaking/detail/users/signed_in_users.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/users/adapters/user_roster.hpp"
#include "src/core/users/server_time.hpp"
#include "src/core/users/usecases/qr_sign_in.hpp"
#include "src/core/users/usecases/sign_in.hpp"

namespace libchiaking::users {

/// @brief Everything one SignedInUsers registry owns: its roster of users, the
/// sign-in use case, the shared PS clock and the fallback random source.
///
/// Built once by the SignedInUsers constructors and destroyed with it. The
/// providers handed to the constructor are referenced, not owned; when no
/// random source is given the core uses its own, so every member is valid from
/// construction. Members are declared in dependency order: the fallback first,
/// then the clocks, then the PS clock, then the roster that every user
/// is built from, then the sign-in use case that writes into the roster, and
/// last the QR sign-in use case that signs in through it.
/// Destruction runs the other way, so the roster wipes every user before the
/// clocks it handed them go away.
///
/// The core adds no locking of its own; the roster and the users guard their
/// own state, so the core is as thread-safe as SignedInUsers promises.
class SignedInUsers::Core {
 public:
  /// @brief Builds the registry's state, falling back to the library's own
  /// random source when none is given.
  ///
  /// @param http Carries every PSN request the registry's users make. Must
  ///     outlive the core.
  /// @param credentials Persists the users. Must outlive the core.
  /// @param threading Makes the users' locks and the waits on shared
  ///     requests. Must outlive the core.
  /// @param clock Monotonic clock for cache lifetimes and the PS clock. Must
  ///     outlive the core.
  /// @param wall Wall clock for token issue and expiry times. Must outlive
  ///     the core.
  /// @param random Generates device ids for saved sign-ins that have none.
  ///     Must outlive the core.
  /// @param log Receives diagnostics, or null to log nothing. Must outlive the
  ///     core when given.
  Core(platform::HttpProvider& http, CredentialStore& credentials,
       platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock,
       const platform::ClockProvider& wall,
       platform::SecureRandomProvider& random,
       platform::DiagnosticsProvider* log);
  /// @brief Not copyable; the core owns the registry's users.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; the core owns the registry's users.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Reaches the set of users the registry holds.
  ///
  /// @return The roster, which lives as long as the core.
  auto roster() noexcept -> UserRoster& { return roster_; }
  /// @brief Reaches the use case that signs users in and loads saved ones.
  ///
  /// @return The use case, which lives as long as the core.
  auto sign_in() noexcept -> usecases::SignIn& { return sign_in_; }
  /// @brief Reaches the use case that signs users in by an approved QR code.
  ///
  /// @return The use case, which lives as long as the core.
  auto qr_sign_in() noexcept -> usecases::QrSignIn& { return qr_sign_in_; }
  /// @brief Reaches the PS clock every user of this registry shares.
  ///
  /// @return The clock, which lives as long as the core.
  auto server_time() const noexcept -> const ServerTime& {
    return server_time_;
  }

 private:
  /// @brief The monotonic clock that drives cache lifetimes and the PS
  /// clock. Fixed at construction.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock that stamps when tokens were obtained and when
  /// they expire. Fixed at construction.
  const platform::ClockProvider& wall_;
  /// @brief PSN's clock as learned from PS's replies, shared by every user
  /// of this registry.
  ServerTime server_time_;
  /// @brief The users the registry currently holds, at most one per account.
  UserRoster roster_;
  /// @brief Signs users in, renews them and loads saved ones into roster_.
  usecases::SignIn sign_in_;
  /// @brief Signs users in by an approved QR code, through sign_in_.
  usecases::QrSignIn qr_sign_in_;
};

}  // namespace libchiaking::users
