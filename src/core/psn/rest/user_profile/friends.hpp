/// @file
/// @brief Declares Friends, the user profile service requests for the user's
/// friends, their profiles and whether they are online.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief One page of the user's friends. Plain data.
struct FriendsPage {
  /// @brief The friends' account ids in decimal, in reply order.
  std::vector<std::string> account_ids;
  /// @brief The `nextOffset`, or nothing on the last page.
  std::optional<std::size_t> next_offset;
};

/// @brief A friend's public profile. Plain data; text fields are empty when
/// PS omitted them.
struct FriendProfile {
  /// @brief The `onlineId`.
  std::string online_id;
  /// @brief The `avatars` entry of size `m`.
  std::string avatar_url;
};

/// @brief Whether a friend is online and what they play. Plain data.
struct FriendPresence {
  /// @brief True when `primaryPlatformInfo.onlineStatus` is `online`.
  bool online{};
  /// @brief The first `gameTitleInfoList` entry's `titleName`, or empty.
  std::string playing;
};

/// @brief Reads the user's friends with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class Friends {
 public:
  /// @brief The failure of presences().
  using PresencesError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of profiles().
  using ProfilesError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of page().
  using PageError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief How many friends one page holds.
  static constexpr std::size_t page_size = 100;

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  Friends(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Reads one page of the user's friends.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userProfile/v2/internal/users/me/friends`
  /// with `limit`, `offset`, `order=availability+realName+onlineId` and
  /// `platforms=PS4,PS5`.
  ///
  /// @param offset Index of the first friend on the page.
  /// @param language The locale to send.
  /// @return The page.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto page(std::size_t offset, std::string_view language)
      -> Expected<FriendsPage, PageError>;

  /// @brief Reads the profiles of up to page_size accounts.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userProfile/v1/internal/users/profiles`
  /// with the ids comma-joined in `accountIds`; the reply's `profiles` match
  /// the ids by position.
  ///
  /// @param account_ids The accounts; at most page_size are read.
  /// @param language The locale to send.
  /// @return One profile for each id read, in the same order; nothing is asked
  ///     for no ids.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto profiles(std::span<const std::string> account_ids,
                              std::string_view language)
      -> Expected<std::vector<FriendProfile>, ProfilesError>;

  /// @brief Reads whether up to page_size accounts are online.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/userProfile/v2/internal/users/basicPresences`
  /// with `accountIds`, `platforms=PS4,PS5`, `type=primary` and
  /// `withOwnGameTitleInfo=true`; the reply's `basicPresences` match the ids
  /// by position.
  ///
  /// @param account_ids The accounts; at most page_size are read.
  /// @param language The locale to send.
  /// @return One presence for each id read, in the same order; nothing is
  ///     asked for no ids.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto presences(std::span<const std::string> account_ids,
                               std::string_view language)
      -> Expected<std::vector<FriendPresence>, PresencesError>;

 private:
  /// @brief The user profile service.
  static constexpr std::string_view profile_url =
      "https://m.np.playstation.com/api/userProfile";
  /// @brief The platforms asked about.
  static constexpr std::string_view platforms = "PS4,PS5";

  /// @brief The friends page read.
  static constexpr net::http::Endpoint page_endpoint{
      .label = "psn.user_profile.friends",
      .log_query = true,
  };
  /// @brief The profiles read.
  static constexpr net::http::Endpoint profiles_endpoint{
      .label = "psn.user_profile.friend_profiles",
  };
  /// @brief The presences read.
  static constexpr net::http::Endpoint presences_endpoint{
      .label = "psn.user_profile.friend_presences",
  };

  /// @brief The friends page reply.
  struct Page;
  /// @brief The profiles reply.
  struct Profiles;
  /// @brief The presences reply.
  struct Presences;

  /// @brief Joins up to page_size ids with commas.
  ///
  /// @param account_ids The ids.
  /// @return The joined ids.
  [[nodiscard]] static auto joined(std::span<const std::string> account_ids)
      -> std::string;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
