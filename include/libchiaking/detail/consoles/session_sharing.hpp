/// @file
/// @brief Declares the values used to share the user's game session and to
/// invite friends to it or to a party: OwnGameSession, SessionLink,
/// InvitationTarget, PsnFriend and GamePlatform.
#pragma once

#include "libchiaking/detail/users/account_id.hpp"

#include <cstdint>
#include <string>

namespace libchiaking::consoles {

/// @brief The console a game in a game session is played on.
///
/// @see KnownConsoles::own_game_session
enum class GamePlatform : std::uint8_t {
  /// @brief A PS4 game.
  ps4,
  /// @brief A PS5 game.
  ps5,
};

/// @brief A game session the user plays in.
///
/// @see KnownConsoles::own_game_session
struct OwnGameSession {
  /// @brief The session manager's session id.
  std::string session_id;
  /// @brief The session's name as the game set it, or empty.
  std::string name;
  /// @brief The players in the session.
  std::uint32_t players{};
  /// @brief The most players the session allows, or zero when PSN did not
  /// say.
  std::uint32_t max_players{};
  /// @brief Whether anyone may join it, the only kind a share link is offered
  /// for.
  bool open_to_anyone{};
};

/// @brief A link friends can open to join a game session, as the mobile
/// app's Share link gives it.
///
/// @see KnownConsoles::share_game_session
struct SessionLink {
  /// @brief The link friends open to join.
  std::string url;
  /// @brief A picture of the link, such as a QR code, or empty when PSN gave
  /// none.
  std::string image_url;
};

/// @brief What an invitation the user sends is to.
///
/// @see KnownConsoles::invite_friends
struct InvitationTarget {
  /// @brief The kind of session.
  enum class Kind : std::uint8_t {
    /// @brief A game session the user plays in.
    game,
    /// @brief A party the user is in.
    party,
  };

  /// @brief The kind of session.
  Kind kind{Kind::game};
  /// @brief The session manager's session or party id.
  std::string session_id;
};

/// @brief One of the user's PSN friends.
///
/// @see KnownConsoles::friends
struct PsnFriend {
  /// @brief The friend's account.
  AccountId account_id{0};
  /// @brief The friend's online id, or empty when PSN did not give it.
  std::string online_id;
  /// @brief The friend's avatar picture, or empty.
  std::string avatar_url;
  /// @brief Whether the friend is online now.
  bool online{};
  /// @brief The game the friend is playing, or empty.
  std::string playing;
};

}  // namespace libchiaking::consoles
