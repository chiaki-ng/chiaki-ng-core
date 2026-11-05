#include "libchiaking/detail/play/launch_refused.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::play {

auto LaunchRefused::not_cleared(catalog::LaunchRefusal refusal)
    -> LaunchRefused {
  return LaunchRefused{NotCleared{std::move(refusal)}};
}

auto LaunchRefused::privacy_settings(std::string_view upgrade_url)
    -> std::optional<LaunchRefused> {
  const auto visible = [](char value) { return value > ' ' && value < 0x7f; };
  if (upgrade_url.size() > url_limit ||
      !std::ranges::all_of(upgrade_url, visible))
    return std::nullopt;
  return LaunchRefused{PrivacySettings{std::string{upgrade_url}}};
}

auto LaunchRefused::licence_not_free(std::string_view display_price)
    -> std::optional<LaunchRefused> {
  if (display_price.size() > price_limit ||
      std::ranges::any_of(display_price, text::ascii_control))
    return std::nullopt;
  return LaunchRefused{LicenceNotFree{std::string{display_price}}};
}

auto LaunchRefused::membership_rejected() -> LaunchRefused {
  return LaunchRefused{MembershipRejected{}};
}

auto LaunchRefused::outside_streaming_plan() -> LaunchRefused {
  return LaunchRefused{OutsideStreamingPlan{}};
}

auto LaunchRefused::store_region() -> LaunchRefused {
  return LaunchRefused{StoreRegion{}};
}

auto LaunchRefused::not_built() -> LaunchRefused {
  return LaunchRefused{NotBuilt{}};
}

auto LaunchRefused::title_not_found() -> LaunchRefused {
  return LaunchRefused{TitleNotFound{}};
}

auto LaunchRefused::refused_by_service(catalog::StreamRefusal::Kind refusal)
    -> LaunchRefused {
  return LaunchRefused{RefusedByService{refusal}};
}

auto LaunchRefused::no_reachable_datacenter(std::optional<RoundTrip> best)
    -> LaunchRefused {
  if (best && best->count() <= 0) best.reset();
  return LaunchRefused{NoReachableDatacenter{best}};
}

auto LaunchRefused::datacenter_unavailable(DatacenterName name)
    -> LaunchRefused {
  return LaunchRefused{DatacenterUnavailable{std::move(name)}};
}

auto LaunchRefused::no_server(NoServer why) -> LaunchRefused {
  return LaunchRefused{NoServerFound{why}};
}

auto LaunchRefused::sign_in_expired() -> LaunchRefused {
  return LaunchRefused{SignInExpired{}};
}

auto LaunchRefused::unavailable(std::uint16_t http_status) -> LaunchRefused {
  return LaunchRefused{Unavailable{http_status}};
}

}  // namespace libchiaking::play
