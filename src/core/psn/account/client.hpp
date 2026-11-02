/// @file
/// @brief Declares account::Client, which reads a PSN account's public profile
/// (online id, avatar, PlayStation Plus flag) with the app token.
///
/// It is the narrow door the users context reads a signed-in user's profile
/// through: it takes the platform's HTTP provider rather than an internal HTTP
/// client, and reduces every REST failure to a small account::Error.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/user_profile/account_profile.hpp"
#include "src/core/psn/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::platform {
/// @brief The host's provider factory.
class PlatformFactory;
}  // namespace libchiaking::platform

namespace libchiaking::psn::account {

/// @brief Why a profile could not be read.
enum class Error : std::uint8_t {
  /// @brief The app token is empty or too long, or the request could not be
  /// built; nothing was sent.
  invalid_argument,
  /// @brief The stop token fired.
  cancelled,
  /// @brief The profile service refused the app token (HTTP 401 or 403); the
  /// caller should record the refusal against the token.
  auth,
  /// @brief The profile service answered HTTP 429.
  rate_limited,
  /// @brief The request did not complete, or the service answered with
  /// another error status.
  unavailable,
  /// @brief The reply was not the expected JSON, had no online id, or its
  /// avatar list was incomplete or named an unsafe URL.
  malformed,
};

/// @brief A failure to read a profile. Plain data.
struct Failure {
  /// @brief Why.
  Error error{Error::unavailable};
  /// @brief The reply's HTTP status; 0 when there was no error reply.
  std::uint16_t http_status{};
};

/// @brief Whether the profile reports PlayStation Plus.
using PlusStatus = psn::PlusStatus;

/// @brief A PSN account's profile as the users context keeps it. Plain data.
struct Profile {
  /// @brief The account's online id; never empty.
  std::string online_id;
  /// @brief The avatar at the medium size, else large, else small, else the
  /// first listed; always HTTPS (an `http` URL on the default port is
  /// upgraded); nothing when the profile has no avatar.
  std::optional<std::string> avatar_url;
  /// @brief The profile's `isPlus` flag; it does not name the tier.
  PlusStatus plus{PlusStatus::unknown};
};

/// @brief Reads account profiles over the host's HTTP provider.
///
/// Holds references to the provider and the log, which must outlive it, and a
/// copy of a stop token. Each call is independent, so one Client may be used
/// from several threads when the provider allows it.
class Client {
 public:
  /// @brief The longest token accepted, in bytes.
  static constexpr std::size_t token_limit = auth::token_maximum_bytes;

  /// @brief Binds the reads to a provider.
  ///
  /// @param http The host's HTTP provider; must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param stop Cancels every read.
  /// @param log Where requests are logged; may be null; must outlive this
  ///     object.
  explicit Client(platform::HttpProvider& http,
                  const platform::ClockProvider& clock,
                  platform::StopToken stop = {},
                  platform::DiagnosticsProvider* log = nullptr) noexcept;
  /// @brief Binds the reads to a platform's HTTP provider and diagnostics.
  ///
  /// @param platform The host's providers; its HTTP provider and diagnostics
  ///     must outlive the client.
  /// @param stop Cancels every read.
  /// @return The client, or nothing when the platform has no HTTP provider.
  [[nodiscard]] static auto on(platform::PlatformFactory& platform,
                               platform::StopToken stop = {})
      -> std::optional<Client>;

  /// @brief Reads an account's profile.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userProfile/v1/internal/users/<account_id>/profiles`
  /// with the app token as a bearer token; `onlineId`, `isPlus` and `avatars`
  /// are read from the JSON reply. Blocks for one round trip.
  ///
  /// @param token The app token.
  /// @param account The account whose profile is read.
  /// @return The profile.
  /// @retval Error::invalid_argument The token is empty or longer than
  ///     token_limit.
  /// @retval Error::cancelled The stop token fired.
  /// @retval Error::auth HTTP 401 or 403.
  /// @retval Error::rate_limited HTTP 429.
  /// @retval Error::unavailable Another failure to send or another error
  ///     status.
  /// @retval Error::malformed The reply could not be read.
  [[nodiscard]] auto profile(const auth::AppToken& token, AccountId account)
      -> Expected<Profile, Failure>;

 private:
  /// @brief HTTP 429 Too Many Requests.
  static constexpr std::uint16_t http_too_many_requests = 429;

  /// @brief Reduces a REST failure.
  ///
  /// @param error The failure.
  /// @return cancelled for a cancelled request; invalid_argument for a
  ///     request that could not be built; auth for 401 or 403; rate_limited
  ///     for 429; unavailable for other statuses and transport failures;
  ///     malformed for an unreadable reply. The HTTP status is carried over.
  static auto failure_of(
      const rest::AccountProfile::DetailsError& error) noexcept -> Failure;

  /// @brief Checks a token before it is sent.
  ///
  /// @tparam Token A credential type with `empty()` and `view()`.
  /// @param token The token.
  /// @return True when it is not empty and at most token_limit bytes.
  template <class Token>
  static auto usable(const Token& token) noexcept -> bool {
    return !token.empty() && token.view().size() <= Client::token_limit;
  }

  /// @brief The provider requests go through.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Cancels every read.
  platform::StopToken stop_;
  /// @brief Where requests are logged; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::psn::account
