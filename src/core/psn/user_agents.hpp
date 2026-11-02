/// @file
/// @brief The user agents the library presents itself to PS with.
///
/// PS routes and polices cloud requests by user agent, so each streaming
/// service gets its own agent: one for PS5 cloud streaming and one for
/// LegacyCloudStream.
#pragma once

#include <string_view>

namespace libchiaking::psn {

/// @brief The user agent sent on PS5 cloud streaming authorisation, GK
/// and launch requests; PS expects exactly this value.
inline constexpr std::string_view cloud_stream_http_user_agent =
    "PlayStation Portal/6.0.0-rel.444+6a9cea6f5";
/// @brief The Electron user agent sent on LegacyCloudStream
/// authorisation, GK, commerce, accounts and public GraphQL requests.
inline constexpr std::string_view legacy_cloud_stream_http_user_agent =
    "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
    "Gecko) "
    "playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 "
    "gkApollo";

}  // namespace libchiaking::psn
