/// @file
/// @brief Umbrella header for the consoles context: Console, KnownConsoles and
/// every value type they use.
#pragma once

namespace libchiaking::consoles {

/// @brief The consoles the library knows; finds and pairs them. Get it from
/// ChiakiNg::consoles.
class KnownConsoles;
/// @brief One PS4 or PS5 the library knows, paired, saved or only seen on the
/// network.
class Console;
/// @brief The title a console reported as running when it last answered.
struct RunningTitle;
/// @brief An invitation to a game session, read from the account's PSN
/// notifications.
struct GameInvitation;
/// @brief Why joining an invitation failed.
struct InvitationJoinFailure;
/// @brief A message a friend sent the user, as PSN announces it.
struct MessageAlert;
/// @brief An invitation to a friend's party, as PSN pushes it.
struct PartyInvitationAlert;
/// @brief Told of what arrives for the user while KnownConsoles::watch_alerts
/// runs.
class AlertWatcher;
/// @brief A game session the user plays in, which friends can be invited to.
struct OwnGameSession;
/// @brief A link friends can open to join the user's game session.
struct SessionLink;
/// @brief The session or party an invitation the user sends is to.
struct InvitationTarget;
/// @brief One of the user's PSN friends.
struct PsnFriend;

}  // namespace libchiaking::consoles

#include "libchiaking/consoles_types.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/users.hpp"
