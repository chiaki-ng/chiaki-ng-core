/// @file
/// @brief Umbrella header for the catalog context: the Catalog service, its
/// value types and the users context it depends on.
#pragma once

namespace libchiaking::catalog {

/// @brief Which games a user can stream from the cloud and whether one may
/// launch now. Get it from ChiakiNg::catalog.
class Catalog;
/// @brief A shown cloud game the account played recently.
struct RecentlyPlayedGame;
/// @brief Where one capture in the account's cloud media gallery can be
/// downloaded.
struct CloudCaptureFile;
/// @brief Where an account's full catalog fetch stands.
class FetchState;

}  // namespace libchiaking::catalog

#include "libchiaking/catalog_types.hpp"
#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/users.hpp"
