/// @file
/// @brief The fixed web identity of the PlayStation Now app and its Kamaji
/// store: origin, referer, OAuth redirect URIs, OAuth client id and the
/// success code its replies carry.
///
/// PS accepts LegacyCloudStream requests only when they look like the
/// LegacyCloudStream web app version 2.2.0 build 133, so the authorisation
/// server, the Kamaji store and the GK launch all send the same values; they
/// are kept here once.
#pragma once

#include <string_view>

namespace libchiaking::psn {

/// @brief The LegacyCloudStream web app's origin, sent as the `Origin` header
/// to the Kamaji store.
inline constexpr std::string_view legacy_cloud_stream_origin =
    "https://psnow.playstation.com";
/// @brief The LegacyCloudStream web app's page, sent as the `Referer` header to
/// the Kamaji store; lies under legacy_cloud_stream_origin.
inline constexpr std::string_view legacy_cloud_stream_referer =
    "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/";
/// @brief The OAuth redirect URI registered for the LegacyCloudStream client,
/// sent with every LegacyCloudStream authorisation request, as the Kamaji
/// `X-Alt-Referer` header and in the LegacyCloudStream launch specification;
/// lies under legacy_cloud_stream_referer.
inline constexpr std::string_view legacy_cloud_stream_redirect_uri =
    "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html";
static_assert(
    legacy_cloud_stream_referer.starts_with(legacy_cloud_stream_origin));
static_assert(
    legacy_cloud_stream_redirect_uri.starts_with(legacy_cloud_stream_referer));
/// @brief The OAuth redirect URI registered for the PS5 cloud streaming
/// client; the authorisation server redirects to it with the code in
/// the `Location` header, and it is never followed.
inline constexpr std::string_view cloud_stream_redirect_uri = "gaikai://local";

/// @brief The LegacyCloudStream OAuth client id, used for LegacyCloudStream
/// authorisation codes and tokens and sent as `client_id` when a Kamaji session
/// is opened.
inline constexpr std::string_view legacy_cloud_stream_client_id =
    "bc6b0777-abb5-40da-92ca-e133cf18e989";
/// @brief The `header.status_code` a Kamaji reply carries when the store
/// accepted the request; any other non-empty code is a refusal.
inline constexpr std::string_view kamaji_status_ok = "0x0000";
/// @brief The `header.status_code` a Kamaji checkout reply carries when the
/// product is sold in another store region than the account's ("Against
/// Eligibility Rule").
inline constexpr std::string_view kamaji_status_ineligible = "0x0113";
/// @brief The browser user agent sent to the Kamaji store, the
/// LegacyCloudStream store lists, the LegacyCloudStream token request and the
/// sign-in authorisation request.
inline constexpr std::string_view kamaji_user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

}  // namespace libchiaking::psn
