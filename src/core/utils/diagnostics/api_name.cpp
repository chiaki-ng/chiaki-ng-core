#include "src/core/utils/diagnostics/api_name.hpp"

#include "src/core/net/nat/stun/public_servers.hpp"

#include <algorithm>
#include <array>

namespace libchiaking::diagnostics {

auto ApiName::resolve(const platform::HttpRequest& request) noexcept
    -> std::string_view {
  if (!request.label.empty()) {
    const auto entries = known();
    const auto entry =
        std::ranges::find(entries, request.label, &ApiName::Entry::label);
    if (entry != entries.end()) return entry->name;
  }
  if (request.label.starts_with("psn.graphql.mobile."))
    return "PSN Mobile GraphQL query";
  if (request.label.starts_with("psn.graphql.web."))
    return "PSN Web GraphQL query";
  if (request.url.view() == net::nat::stun::PublicServers::host_list_url ||
      request.url.view() == net::nat::stun::PublicServers::ipv6_host_list_url)
    return "STUN Get preferred servers";
  return "Unclassified HTTP API";
}

ApiName::ApiName(const platform::HttpRequest& request) noexcept
    : name_(resolve(request)) {}

auto ApiName::known() noexcept -> std::span<const Entry> {
  static constexpr std::array entries{
      ApiName::Entry{"psn.session_manager.create_remote_play_session",
                     "PSN Create Remote Play session"},
      ApiName::Entry{"psn.session_manager.observe_sessions",
                     "PSN Read Remote Play session"},
      ApiName::Entry{"psn.session_manager.leave_session",
                     "PSN Release Remote Play membership"},
      ApiName::Entry{"psn.session_manager.send_session_message",
                     "PSN Send session signalling message"},
      ApiName::Entry{"psn.cloud_assisted_navigation.start_remote_play",
                     "PSN Start PS5 Remote Play"},
      ApiName::Entry{"psn.cloud_assisted_navigation.launch_title",
                     "PSN Launch title"},
      ApiName::Entry{"psn.cloud_assisted_navigation.poll_command",
                     "PSN Poll console command"},
      ApiName::Entry{"psn.cloud_assisted_navigation.list_clients",
                     "PSN List consoles"},
      ApiName::Entry{"psn.cloud_assisted_navigation.list_cloud_clients",
                     "PSN Find cloud console"},
      ApiName::Entry{"psn.cloud_assisted_navigation.send_game_intent",
                     "PSN Join game session"},
      ApiName::Entry{"psn.cloud_assisted_navigation.join_party",
                     "PSN Join party"},
      ApiName::Entry{"psn.session_manager.player_sessions",
                     "PSN Read game sessions"},
      ApiName::Entry{"psn.user_notification.list", "PSN List notifications"},
      ApiName::Entry{"psn.gaming_lounge.groups", "PSN List conversations"},
      ApiName::Entry{"psn.gaming_lounge.messages", "PSN Read messages"},
      ApiName::Entry{"psn.gaming_lounge.send", "PSN Send message"},
      ApiName::Entry{"psn.gaming_lounge.mark_read",
                     "PSN Mark conversation read"},
      ApiName::Entry{"psn.gamelist.played_titles", "PSN List played titles"},
      ApiName::Entry{"psn.trophy.summary", "PSN Read trophy summary"},
      ApiName::Entry{"psn.trophy.titles", "PSN List trophy titles"},
      ApiName::Entry{"psn.trophy.groups", "PSN List trophy groups"},
      ApiName::Entry{"psn.trophy.earned_groups",
                     "PSN Read earned trophy groups"},
      ApiName::Entry{"psn.trophy.trophies", "PSN List trophies"},
      ApiName::Entry{"psn.trophy.earned_trophies", "PSN Read earned trophies"},
      ApiName::Entry{"psn.trophy.title_sets", "PSN Map titles to trophy sets"},
      ApiName::Entry{"psn.game_media.capture", "PSN Find cloud capture"},
      ApiName::Entry{"psn.game_media.download_address",
                     "PSN Get cloud capture download address"},
      ApiName::Entry{"psn.settings.setting", "PSN Read account setting"},
      ApiName::Entry{"psn.entitlement.library_concepts",
                     "PSN List owned game concepts"},
      ApiName::Entry{"psn.entitlement.library_games",
                     "PSN List owned PS5 games"},
      ApiName::Entry{"psn.entitlement.library_game",
                     "PSN Get one owned PS5 game"},
      ApiName::Entry{"psn.user_profile.profile_base_url",
                     "PSN Resolve PS4 profile service"},
      ApiName::Entry{"psn.user_profile.wake_ps4", "PSN Wake PS4"},
      ApiName::Entry{"psn.push.server_address", "PSN Resolve push server"},
      ApiName::Entry{"gk.client_ids", "Cloud Get client IDs"},
      ApiName::Entry{"gk.config", "Cloud Get configuration"},
      ApiName::Entry{"gk.start", "Cloud Start session"},
      ApiName::Entry{"gk.authorize", "Cloud Authorize entitlement"},
      ApiName::Entry{"gk.lock", "Cloud Acquire session lock"},
      ApiName::Entry{"gk.datacenters", "Cloud List datacenters"},
      ApiName::Entry{"gk.select", "Cloud Select datacenter"},
      ApiName::Entry{"gk.allocate", "Cloud Allocate instance"},
      ApiName::Entry{"psn.auth.authorize_check", "Cloud Check authorization"},
      ApiName::Entry{"psn.auth.commerce_token", "PS OAuth authorization"},
      ApiName::Entry{"psn.auth.legacy_cloud_stream_code",
                     "PS OAuth authorization"},
      ApiName::Entry{"psn.auth.gk_client_code", "PS OAuth authorization"},
      ApiName::Entry{"psn.auth.gk_server_code", "PS OAuth authorization"},
      ApiName::Entry{"psn.kamaji.create_session",
                     "LegacyCloudStream Create catalog session"},
      ApiName::Entry{"psn.kamaji.buynow_preview",
                     "LegacyCloudStream Preview entitlement resolution"},
      ApiName::Entry{"psn.kamaji.buynow",
                     "LegacyCloudStream Resolve entitlement"},
      ApiName::Entry{"psn.kamaji.stores",
                     "LegacyCloudStream Resolve catalog title"},
      ApiName::Entry{"psn.kamaji.container",
                     "LegacyCloudStream Resolve catalog title"},
      ApiName::Entry{"psn.kamaji.category",
                     "LegacyCloudStream Resolve catalog title"},
      ApiName::Entry{"psn.commerce.internal_entitlements",
                     "LegacyCloudStream List owned entitlements"},
      ApiName::Entry{"psn.commerce.internal_entitlement",
                     "LegacyCloudStream Get owned entitlements"},
      ApiName::Entry{"psn.accounts.validate_attributes",
                     "LegacyCloudStream Get account attributes"},
      ApiName::Entry{"psn.store.product_page_exists",
                     "PSN Check store product page"},
      ApiName::Entry{"psn.store.games_lists",
                     "LegacyCloudStream Get games lists"},
      ApiName::Entry{"psn.graphql.mobile.metGetConceptById",
                     "PSN Get title concept"},
      ApiName::Entry{"psn.graphql.mobile.metGetPricingDataByConceptId",
                     "PSN Get title pricing"},
      ApiName::Entry{"psn.graphql.mobile.metGetConceptByProductIdQuery",
                     "PSN Find title concept by product"},
      ApiName::Entry{"psn.graphql.web.categoryGridRetrieve",
                     "PS Plus Get catalog category"},
      ApiName::Entry{"psn.graphql.web.conceptRetrieveForMedia",
                     "PS Store Get game artwork"},
      ApiName::Entry{"psn.graphql.web.wcaConceptStarRatingRetrive",
                     "PS Store Get star rating"},
      ApiName::Entry{"psn.graphql.web.conceptRetrieveForContentRating",
                     "PS Store Get age rating"},
      ApiName::Entry{"psn.graphql.web.conceptRetrieveForGameInfo",
                     "PS Store Get game info"},
      ApiName::Entry{"psn.graphql.web.getSubscriptions",
                     "PSN Get subscriptions"},
  };
  return entries;
}

}  // namespace libchiaking::diagnostics
