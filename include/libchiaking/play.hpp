/// @file
/// @brief Includes everything needed to start and run play sessions: Play,
/// Session and InternetPairer.
#pragma once

namespace libchiaking::play {

/// @brief Starts play sessions and tracks the live one. Get it from
/// ChiakiNg::play.
class Play;
/// @brief One play session, from the first connection attempt until it has
/// ended.
class Session;
/// @brief Pairs with a console over the internet with the player's PSN account.
class InternetPairer;

}  // namespace libchiaking::play

#include "libchiaking/catalog.hpp"
#include "libchiaking/consoles.hpp"
#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/detail/play/play.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/play_types.hpp"
