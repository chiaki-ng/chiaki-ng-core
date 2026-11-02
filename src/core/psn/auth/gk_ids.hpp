/// @file
/// @brief The shape of the device unique id (DUID) that GK and the
/// authorisation server identify the streaming device by, and the client ids
/// GK assigns to a cloud session.
#pragma once

#include "libchiaking/detail/users/device_id.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace libchiaking::psn {

/// @brief The 16 hex digits every GK DUID starts with; the rest of the
/// DUID is 32 lowercase hex digits of random bytes chosen once per device
/// (users::DeviceId).
inline constexpr std::string_view gk_duid_prefix = users::DeviceId::prefix;
static_assert(gk_duid_prefix.size() == 16);
/// @brief Length of a GK DUID in characters, the prefix included (48).
inline constexpr std::size_t gk_duid_length = users::DeviceId::length;
/// @brief The longest GK client id, entitlement id or event name kept, in
/// bytes; longer values from GK are truncated to it and longer client ids
/// are refused when an authorisation request is built.
inline constexpr std::size_t gk_id_max_length = 127;

}  // namespace libchiaking::psn

namespace libchiaking::psn::auth {

/// @brief The OAuth client ids GK hands out for one cloud session, read
/// from its `clientIds` reply (gk::Client::client_ids()).
///
/// The authorisation server then issues one code to each: the client code
/// proves the user to the streaming client, the server code to the stream
/// server. Plain data; each id is at most gk_id_max_length bytes.
struct GkClientIds {
  /// @brief The `gkClientId`: the client the client-side authorisation code
  /// is issued to; never empty in a decoded reply.
  std::string gk_client_id;
  /// @brief The `ps3GkClientId`: the client the server-side code is issued to
  /// on LegacyCloudStream; empty when GK sent none.
  std::string ps3_gk_client_id;
  /// @brief The `streamServerClientId`: the client the server-side code is
  /// issued to on PS5 cloud streaming; empty when GK sent none.
  std::string stream_server_client_id;
};

}  // namespace libchiaking::psn::auth
