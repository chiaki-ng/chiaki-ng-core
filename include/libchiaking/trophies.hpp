/// @file
/// @brief Umbrella header for the trophies context: the Trophies service, its
/// value types and the users context it depends on.
#pragma once

namespace libchiaking::trophies {

/// @brief A signed-in user's trophies, read from PSN. Get it from
/// ChiakiNg::trophies.
class Trophies;

}  // namespace libchiaking::trophies

#include "libchiaking/detail/trophies/trophies.hpp"
#include "libchiaking/trophies_types.hpp"
#include "libchiaking/users.hpp"
