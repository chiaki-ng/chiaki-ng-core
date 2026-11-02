/// @file
/// @brief Declares AccountProfile, the PSN user profile service's view of one
/// account's public profile, read with the app token.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace libchiaking::psn::rest {

/// @brief The largest profile reply accepted, in bytes.
inline constexpr std::size_t account_profile_response_limit = 16'000'000;

/// @brief Whether the profile reports PlayStation Plus.
using PlusStatus = psn::PlusStatus;

/// @brief An account's profile. Plain data.
struct ProfileDetails {
  /// @brief The `onlineId`; never empty.
  std::string online_id;
  /// @brief The avatar chosen as account::Profile describes; nothing when the
  /// profile lists none.
  std::optional<std::string> avatar_url;
  /// @brief The `isPlus` flag; unknown when absent.
  PlusStatus plus_status{PlusStatus::unknown};
};

/// @brief Reads account profiles with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class AccountProfile {
 public:
  /// @brief The failure of details().
  using DetailsError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema,
                             bad_reply::InvalidUrl>;
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  AccountProfile(net::http::Client& client, auth::AppToken token) noexcept;

  /// @brief Reads an account's profile.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userProfile/v1/internal/users/<account_id>/profiles`
  /// accepting any media type; `onlineId`, `isPlus` and `avatars` (each a
  /// `size` and `url`) are read.
  ///
  /// @param account The account.
  /// @return The profile.
  /// @retval net::http::Failure The token is unusable (an invalid request),
  ///     or the request failed.
  /// @retval bad_reply::InvalidSchema No online id, or an avatar lacks
  ///     its size or URL.
  /// @retval bad_reply::InvalidUrl The chosen avatar URL is not a safe
  ///     HTTP(S) URL.
  /// @retval Error An error status or an unreadable reply.
  [[nodiscard]] auto details(AccountId account)
      -> Expected<ProfileDetails, DetailsError>;

 private:
  /// @brief The fault of avatar_url().
  using AvatarFault = Fault<bad_reply::InvalidSchema, bad_reply::InvalidUrl>;
  /// @brief The profiles collection; the account id and `/profiles` follow.
  static constexpr std::string_view profiles_url =
      "https://m.np.playstation.com/api/userProfile/v1/internal/users/";
  /// @brief HTTPS.
  static constexpr std::string_view secure_scheme = "https";
  /// @brief HTTP, accepted only to be upgraded.
  static constexpr std::string_view plain_scheme = "http";
  /// @brief What follows the scheme in a URL.
  static constexpr std::string_view scheme_separator = "://";
  /// @brief The longest avatar URL accepted, in bytes.
  static constexpr std::size_t avatar_url_max_bytes = 4096;
  /// @brief The HTTP port an `http` avatar URL may name to be upgraded.
  static constexpr unsigned plain_default_port = 80;
  /// @brief The avatar sizes preferred, best first.
  static constexpr std::array<std::string_view, 3> preferred_avatar_sizes{
      "m", "l", "s"};

  /// @brief The profile request.
  static constexpr net::http::Endpoint details_endpoint{
      .label = "psn.user_profile.details",
      .maximum_response_bytes = account_profile_response_limit,
  };

  /// @brief The profile reply's fields.
  struct Reply;

  /// @brief Makes an avatar URL safe to show.
  ///
  /// @param url The URL.
  /// @return An HTTPS URL as given; an `http` URL on no port or port 80
  ///     rewritten to `https`; nothing for an empty URL, one over 4096 bytes,
  ///     one with a blank, control byte or `\`, another scheme or another
  ///     port.
  [[nodiscard]] static auto secure_avatar_url(std::string_view url)
      -> std::optional<std::string>;
  /// @brief Chooses the avatar.
  ///
  /// @param reply The profile reply.
  /// @return The medium, else large, else small, else first avatar's safe
  ///     URL; nothing when there are no avatars.
  /// @retval bad_reply::InvalidSchema An avatar lacks its size or URL.
  /// @retval bad_reply::InvalidUrl The chosen URL is not safe.
  [[nodiscard]] static auto avatar_url(const Reply& reply)
      -> Expected<std::optional<std::string>, AvatarFault>;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::AppToken token_;
};

}  // namespace libchiaking::psn::rest
