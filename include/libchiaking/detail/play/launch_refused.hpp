/// @file
/// @brief Declares LaunchRefused, the reason a cloud launch could not go ahead.
#pragma once

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/play/datacenter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::play {

/// @brief Says why a title could not be launched on the cloud, and what would
/// help.
///
/// Each kind carries only the detail that belongs to it; the other accessors
/// return empty values. A refusal reaches the frontend through
/// Outcome::launch_refused when the launch fails on the session's thread, or
/// through StartRefusal::launch when Play::start_on_cloud cannot even begin.
class LaunchRefused {
 public:
  /// @brief Round-trip time, in whole milliseconds.
  using RoundTrip = std::chrono::milliseconds;

  /// @brief Names the reason for the refusal.
  enum class Kind : std::uint8_t {
    /// @brief The catalog did not clear the title for this account;
    /// LaunchRefused::clearance says why.
    not_cleared,
    /// @brief The account's privacy settings must be reviewed first;
    /// LaunchRefused::upgrade_url is the page for it.
    privacy_settings,
    /// @brief The title's licence is not free for this account;
    /// LaunchRefused::display_price is what it costs.
    licence_not_free,
    /// @brief The service rejected the account's PlayStation Plus
    /// membership.
    membership_rejected,
    /// @brief The service does not know the title.
    title_not_found,
    /// @brief PS's streaming service refused the title;
    /// LaunchRefused::ps_refusal says how.
    refused_by_service,
    /// @brief No datacenter could be reached;
    /// LaunchRefused::best_round_trip is the best that was seen.
    no_reachable_datacenter,
    /// @brief The datacenter the player forced is not available;
    /// LaunchRefused::datacenter names it.
    datacenter_unavailable,
    /// @brief No server could be obtained; LaunchRefused::no_server says
    /// why.
    no_server,
    /// @brief The service did not accept the account's sign-in; the player
    /// needs to sign in again.
    sign_in_expired,
    /// @brief The launch failed for another reason, or was cancelled;
    /// LaunchRefused::http_status may say more.
    unavailable,
    /// @brief The title is not part of the streaming plan the membership
    /// includes.
    outside_streaming_plan,
    /// @brief The title's edition is sold in another PlayStation Store region
    /// than the account's, so the store will not license it to the account.
    store_region,
  };

  /// @brief Names why no server was obtained.
  enum class NoServer : std::uint8_t {
    /// @brief The wait for a free server ran out.
    queue_timed_out,
    /// @brief Network errors kept the client from reaching the service.
    network,
  };

  /// @brief Longest upgrade URL accepted, in bytes.
  static constexpr std::size_t url_limit = 2047;
  /// @brief Longest display price accepted, in bytes.
  static constexpr std::size_t price_limit = 64;

  /// @brief Builds a refusal from the catalog's verdict.
  ///
  /// @param refusal Why the catalog did not clear the title.
  ///
  /// @return The Kind::not_cleared refusal.
  static auto not_cleared(catalog::LaunchRefusal refusal) -> LaunchRefused;
  /// @brief Builds a refusal that asks the player to review their privacy
  /// settings.
  ///
  /// @param upgrade_url Page where the settings can be reviewed; may be
  ///     empty.
  /// @return The refusal, or `std::nullopt` when the URL is longer than
  ///     LaunchRefused::url_limit or contains a space, control or non-ASCII
  ///     character.
  [[nodiscard]] static auto privacy_settings(std::string_view upgrade_url)
      -> std::optional<LaunchRefused>;
  /// @brief Builds a refusal for a title whose licence must be paid for.
  ///
  /// @param display_price Price as the store formats it; may be empty.
  /// @return The refusal, or `std::nullopt` when the price is longer than
  ///     LaunchRefused::price_limit or contains a control character.
  [[nodiscard]] static auto licence_not_free(std::string_view display_price)
      -> std::optional<LaunchRefused>;
  /// @brief Builds a Kind::membership_rejected refusal.
  ///
  /// @return The refusal.
  static auto membership_rejected() -> LaunchRefused;
  /// @brief Builds a Kind::outside_streaming_plan refusal.
  ///
  /// @return The refusal.
  static auto outside_streaming_plan() -> LaunchRefused;
  /// @brief Builds a Kind::store_region refusal.
  ///
  /// @return The refusal.
  static auto store_region() -> LaunchRefused;
  /// @brief Builds a Kind::title_not_found refusal.
  ///
  /// @return The refusal.
  static auto title_not_found() -> LaunchRefused;
  /// @brief Builds a refusal for a title PS's streaming service turned
  /// down.
  ///
  /// @param refusal How the service turned it down.
  ///
  /// @return The Kind::refused_by_service refusal.
  static auto refused_by_service(catalog::StreamRefusal::Kind refusal)
      -> LaunchRefused;
  /// @brief Builds a refusal for a launch that reached no datacenter.
  ///
  /// @param best Best round trip seen; ignored unless positive.
  ///
  /// @return The Kind::no_reachable_datacenter refusal.
  static auto no_reachable_datacenter(std::optional<RoundTrip> best)
      -> LaunchRefused;
  /// @brief Builds a refusal for a forced datacenter that is not available.
  ///
  /// @param name Datacenter the player forced.
  ///
  /// @return The Kind::datacenter_unavailable refusal.
  static auto datacenter_unavailable(DatacenterName name) -> LaunchRefused;
  /// @brief Builds a refusal for a launch that obtained no server.
  ///
  /// @param why Why none was obtained.
  ///
  /// @return The Kind::no_server refusal.
  static auto no_server(NoServer why) -> LaunchRefused;
  /// @brief Builds a Kind::sign_in_expired refusal.
  ///
  /// @return The refusal.
  static auto sign_in_expired() -> LaunchRefused;
  /// @brief Builds a refusal for a failure with no more specific kind.
  ///
  /// @param http_status HTTP status of the failed request, or 0 when there
  ///     was none.
  ///
  /// @return The Kind::unavailable refusal.
  static auto unavailable(std::uint16_t http_status = 0) -> LaunchRefused;

  /// @brief Returns the reason for the refusal.
  ///
  /// @return The kind the refusal was built as.
  auto kind() const noexcept -> Kind {
    return std::visit([](const auto& detail) { return detail.kind; }, detail_);
  }
  /// @brief Returns the catalog's verdict; set only for Kind::not_cleared.
  ///
  /// The verdict distinguishes, among others, a title that needs a higher
  /// membership tier, one that must be added to the library, one that must be
  /// bought and one that has not been released.
  ///
  /// @return The verdict, or `nullptr` for every other kind.
  auto clearance() const noexcept -> const catalog::LaunchRefusal* {
    const auto* cleared = std::get_if<NotCleared>(&detail_);
    return cleared ? &cleared->refusal : nullptr;
  }
  /// @brief Returns the page for reviewing privacy settings.
  ///
  /// @return The page; empty for every kind but Kind::privacy_settings, and
  ///     possibly empty for that kind too.
  auto upgrade_url() const noexcept -> std::string_view {
    const auto* privacy = std::get_if<PrivacySettings>(&detail_);
    return privacy ? std::string_view{privacy->upgrade_url}
                   : std::string_view{};
  }
  /// @brief Returns the price as the store formats it.
  ///
  /// @return The price; empty for every kind but Kind::licence_not_free, and
  ///     possibly empty for that kind too.
  auto display_price() const noexcept -> std::string_view {
    const auto* priced = std::get_if<LicenceNotFree>(&detail_);
    return priced ? std::string_view{priced->display_price}
                  : std::string_view{};
  }
  /// @brief Returns how PS's service turned the title down.
  ///
  /// @return The service's refusal; set only for Kind::refused_by_service.
  auto ps_refusal() const noexcept
      -> std::optional<catalog::StreamRefusal::Kind> {
    const auto* ps = std::get_if<RefusedByService>(&detail_);
    return ps ? std::optional{ps->refusal} : std::nullopt;
  }
  /// @brief Returns the best round trip seen.
  ///
  /// @return The round trip; set only for Kind::no_reachable_datacenter, and
  ///     only when one was measured.
  auto best_round_trip() const noexcept -> std::optional<RoundTrip> {
    const auto* unreachable = std::get_if<NoReachableDatacenter>(&detail_);
    return unreachable ? unreachable->best : std::nullopt;
  }
  /// @brief Returns the forced datacenter.
  ///
  /// @return The datacenter, or `nullptr` for every kind but
  ///     Kind::datacenter_unavailable.
  auto datacenter() const noexcept -> const DatacenterName* {
    const auto* unavailable = std::get_if<DatacenterUnavailable>(&detail_);
    return unavailable ? &unavailable->name : nullptr;
  }
  /// @brief Returns why no server was obtained.
  ///
  /// @return The reason; set only for Kind::no_server.
  auto no_server() const noexcept -> std::optional<NoServer> {
    const auto* serverless = std::get_if<NoServerFound>(&detail_);
    return serverless ? std::optional{serverless->why} : std::nullopt;
  }
  /// @brief Returns the HTTP status behind Kind::unavailable.
  ///
  /// @return The status, or 0 for every other kind and when there was none.
  auto http_status() const noexcept -> std::uint16_t {
    const auto* unavailable = std::get_if<Unavailable>(&detail_);
    return unavailable ? unavailable->http_status : std::uint16_t{};
  }

  /// @brief Reports whether the catalog is told about this refusal.
  ///
  /// @return `true` for Kind::refused_by_service. The catalog then remembers
  ///     that PS refuses to stream this title for this account.
  auto is_reported_to_catalog() const noexcept -> bool {
    return kind() == Kind::refused_by_service;
  }
  /// @brief Reports whether choosing another datacenter might help.
  ///
  /// @return `true` for Kind::no_reachable_datacenter and
  ///     Kind::datacenter_unavailable.
  auto is_about_datacenters() const noexcept -> bool {
    return kind() == Kind::no_reachable_datacenter ||
           kind() == Kind::datacenter_unavailable;
  }

 private:
  struct NotCleared {
    static constexpr Kind kind = Kind::not_cleared;
    catalog::LaunchRefusal refusal;
  };
  struct PrivacySettings {
    static constexpr Kind kind = Kind::privacy_settings;
    std::string upgrade_url;
  };
  struct LicenceNotFree {
    static constexpr Kind kind = Kind::licence_not_free;
    std::string display_price;
  };
  struct MembershipRejected {
    static constexpr Kind kind = Kind::membership_rejected;
  };
  struct TitleNotFound {
    static constexpr Kind kind = Kind::title_not_found;
  };
  struct RefusedByService {
    static constexpr Kind kind = Kind::refused_by_service;
    catalog::StreamRefusal::Kind refusal;
  };
  struct NoReachableDatacenter {
    static constexpr Kind kind = Kind::no_reachable_datacenter;
    std::optional<RoundTrip> best;
  };
  struct DatacenterUnavailable {
    static constexpr Kind kind = Kind::datacenter_unavailable;
    DatacenterName name;
  };
  struct NoServerFound {
    static constexpr Kind kind = Kind::no_server;
    NoServer why;
  };
  struct SignInExpired {
    static constexpr Kind kind = Kind::sign_in_expired;
  };
  struct Unavailable {
    static constexpr Kind kind = Kind::unavailable;
    std::uint16_t http_status;
  };
  struct OutsideStreamingPlan {
    static constexpr Kind kind = Kind::outside_streaming_plan;
  };
  struct StoreRegion {
    static constexpr Kind kind = Kind::store_region;
  };
  using Detail =
      std::variant<NotCleared, PrivacySettings, LicenceNotFree,
                   MembershipRejected, TitleNotFound, RefusedByService,
                   NoReachableDatacenter, DatacenterUnavailable, NoServerFound,
                   SignInExpired, Unavailable, OutsideStreamingPlan,
                   StoreRegion>;

  explicit LaunchRefused(Detail detail) noexcept : detail_(std::move(detail)) {}

  Detail detail_;
};

}  // namespace libchiaking::play
