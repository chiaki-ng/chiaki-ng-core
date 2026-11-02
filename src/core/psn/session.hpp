/// @file
/// @brief Declares Session, one signed-in user's PSN credentials: the NPSSO and
/// everything derived from it that is worth keeping between requests.
///
/// The users context gives each signed-in user one Session. The credentials
/// themselves stay behind an opaque state; only SessionInternals, inside the
/// psn context, reaches the token cache, the app token slot and the GraphQL
/// statistics.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::psn {

/// @brief The psn context's access to a Session's state.
class SessionInternals;

/// @brief A user's PSN session: the NPSSO, the commerce tokens exchanged from
/// it (auth::TokenCache), the supplied app token (auth::AppTokenSlot) and
/// per-operation GraphQL statistics (graphql::Stats).
///
/// The NPSSO is fixed for the session's life; signing in again makes a new
/// Session. Neither copyable nor movable once made (it is held by pointer).
/// Every member is thread-safe: the parts lock themselves.
class Session {
 public:
  /// @brief Why a credential was not accepted.
  enum class Error : std::uint8_t {
    /// @brief The NPSSO is empty, too long, or holds a byte a header cannot
    /// carry.
    invalid_npsso,
    /// @brief The app token is empty, too long, or holds a byte a header
    /// cannot carry.
    invalid_app_token,
    /// @brief The app token's expiry lies before the Unix epoch.
    invalid_expiry,
  };

  /// @brief The longest NPSSO accepted, in bytes.
  static constexpr std::size_t npsso_limit = auth::NpssoKind::maximum_bytes;
  /// @brief The longest app token accepted, in bytes.
  static constexpr std::size_t app_token_limit =
      auth::AppTokenKind::maximum_bytes;

  /// @brief Opens a session for an NPSSO.
  ///
  /// @param npsso The NPSSO.
  /// @param threading Provider of the session's locks; must outlive the
  ///     session.
  /// @param clock The monotonic clock token lifetimes are measured on; must
  ///     outlive the session.
  /// @param wall The wall clock statistics and the app token expiry use; must
  ///     outlive the session.
  /// @return The session.
  /// @retval Error::invalid_npsso The NPSSO is not valid_npsso().
  [[nodiscard]] static auto open(std::string_view npsso,
                                 platform::ThreadingProvider& threading,
                                 const platform::ClockProvider& clock,
                                 const platform::ClockProvider& wall)
      -> Expected<std::shared_ptr<Session>, Error>;
  /// @brief Checks an NPSSO's form; it does not ask PSN.
  ///
  /// @param npsso The candidate.
  /// @return True when auth::Npsso admits it.
  static auto valid_npsso(std::string_view npsso) noexcept -> bool;

  /// @brief Makes a session on the given providers.
  ///
  /// @param npsso The NPSSO; may be empty for a user that is off PSN.
  /// @param threading Provider of the session's locks; must outlive the
  ///     session.
  /// @param clock The monotonic clock token lifetimes are measured on; must
  ///     outlive the session.
  /// @param wall The wall clock statistics and the app token expiry use; must
  ///     outlive the session.
  Session(auth::Npsso npsso, platform::ThreadingProvider& threading,
          const platform::ClockProvider& clock,
          const platform::ClockProvider& wall);
  /// @brief Not copyable: the session holds secrets.
  Session(const Session&) = delete;
  /// @brief Not copyable: the session holds secrets.
  auto operator=(const Session&) -> Session& = delete;
  /// @brief Wipes every credential.
  ~Session();

  /// @brief Reads the NPSSO.
  ///
  /// @return The NPSSO; valid for the session's life.
  auto npsso() const noexcept -> const auth::Npsso&;
  /// @brief Compares an NPSSO with the session's in constant time.
  ///
  /// @param npsso The candidate.
  /// @return Whether it is the session's NPSSO.
  auto matches(std::string_view npsso) const noexcept -> bool;

  /// @brief Hands the session a newly issued app token, replacing any other.
  ///
  /// @param token The token.
  /// @param expires_at When it expires, by the wall clock; the epoch means no
  ///     stated expiry.
  /// @return Nothing, when the token was taken.
  /// @retval Error::invalid_app_token The token has the wrong form.
  /// @retval Error::invalid_expiry `expires_at` is before the epoch.
  [[nodiscard]] auto supply_app_token(
      std::string_view token, std::chrono::system_clock::time_point expires_at)
      -> Expected<void, Error>;
  /// @brief Drops the app token, keeping its statistics.
  void clear_app_token() noexcept;

  /// @brief Forgets every derived credential and statistic, keeping only the
  /// NPSSO.
  void clear();
  /// @brief Describes the session's credentials for diagnostics, with no
  /// secrets.
  ///
  /// @return The JSON document SessionInternals::snapshot_json() writes, with
  ///     a null subscription; nothing when it could not be written.
  [[nodiscard]] auto snapshot_json() -> std::optional<std::string>;

 private:
  /// @brief Checks an app token's form.
  ///
  /// @param token The candidate.
  /// @return True when auth::AppToken admits it.
  static auto valid_app_token(std::string_view token) noexcept -> bool;

  /// @brief Reaches state_ for the psn context.
  friend class SessionInternals;
  /// @brief The credential caches and the session's creation time.
  class State;

  /// @brief The state; never null.
  std::unique_ptr<State> state_;
};

}  // namespace libchiaking::psn
