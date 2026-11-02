#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"

#include "src/core/net/transport/http/status.hpp"

#include <chrono>
#include <utility>

namespace libchiaking::play {

LaunchFailure::Unmet::Unmet(Precondition precondition,
                            std::optional<catalog::LaunchRefusal> clearance)
    : precondition_(precondition), clearance_(std::move(clearance)) {}

auto LaunchFailure::Unmet::describe() const noexcept -> std::string_view {
  switch (precondition_) {
    case Precondition::invalid_service:
      return "the cloud service is not supported";
    case Precondition::invalid_platform:
      return "the selected platform is not available for this cloud service";
    case Precondition::invalid_npsso:
      return "the PlayStation sign-in token (npsso) is missing";
    case Precondition::invalid_bitrate:
      return "the stream bitrate must be positive";
    case Precondition::invalid_spec:
      return "the title or stream settings could not be encoded";
    case Precondition::invalid_duid:
      return "the client DUID is not a 48-digit hex GK device ID with "
             "the expected prefix";
    case Precondition::game_not_released:
      break;
  }
  return "the game has not been released yet";
}

auto LaunchFailure::Unmet::refused() const -> LaunchRefused {
  return clearance_ ? LaunchRefused::not_cleared(*clearance_)
                    : LaunchRefused::unavailable();
}

LaunchFailure::NotCleared::NotCleared(catalog::LaunchRefusal refusal)
    : refusal_(std::move(refusal)) {}

auto LaunchFailure::NotCleared::unanswered() const noexcept
    -> std::optional<psn::graphql::CheckStatus> {
  using Kind = catalog::LaunchRefusal::Kind;
  using Status = psn::graphql::CheckStatus;
  switch (refusal_.kind()) {
    case Kind::needs_premium:
    case Kind::not_streamable:
    case Kind::refused_by_service:
    case Kind::add_to_library:
    case Kind::buy_to_stream:
    case Kind::invalid_title:
    case Kind::title_taken_down:
    case Kind::streaming_unavailable:
    case Kind::purchase_needed:
    case Kind::unexpected_entitlement_state:
    case Kind::choose_a_game:
      return std::nullopt;
    case Kind::not_listed:
    case Kind::hidden:
      return Status::not_found;
    case Kind::check_unavailable:
      break;
    default:
      return Status::ok;
  }
  const auto& cause = refusal_.cause();
  if (!cause) return Status::unavailable;
  using Reason = catalog::LaunchCheckReason;
  switch (cause->reason()) {
    case Reason::cancelled:
      return Status::cancelled;
    case Reason::rejected:
    case Reason::region_unsupported:
      return Status::rejected;
    case Reason::malformed:
      return Status::malformed;
    case Reason::not_listed:
      return Status::not_found;
    case Reason::user:
      if (cause->cause()) {
        using User = users::Unanswered::Reason;
        switch (cause->cause()->reason) {
          case User::signed_out:
            return Status::no_session;
          case User::sign_in_expired:
            return Status::auth_refused;
          case User::store_access_lapsed:
            return Status::no_app_token;
          case User::rejected:
            return Status::rejected;
          case User::malformed:
            return Status::malformed;
          default:
            break;
        }
      }
      break;
    case Reason::unavailable:
    case Reason::ownership_unknown:
    case Reason::restriction_list_unavailable:
      break;
  }
  return Status::unavailable;
}

auto LaunchFailure::NotCleared::describe() const noexcept -> std::string_view {
  using Kind = catalog::LaunchRefusal::Kind;
  switch (refusal_.kind()) {
    case Kind::needs_premium:
      return "cloud streaming requires an active PlayStation Plus Premium "
             "subscription";
    case Kind::not_streamable:
    case Kind::refused_by_service:
      return "PS does not list this title as supporting cloud streaming";
    case Kind::add_to_library:
      return "the PlayStation Plus catalog title has not been added to the "
             "library";
    case Kind::buy_to_stream:
      return "this edition is not in the library and is not included with "
             "PlayStation Plus";
    case Kind::invalid_title:
      return "PlayStation holds no record of this copy of the title";
    case Kind::title_taken_down:
      return "PlayStation has taken this title down from cloud streaming";
    case Kind::streaming_unavailable:
      return "cloud streaming is not available for this copy of the title";
    case Kind::purchase_needed:
      return "this copy of the title is no longer active and has to be "
             "bought again to stream";
    case Kind::unexpected_entitlement_state:
      return "PlayStation reports this copy in a state that cannot stream";
    case Kind::choose_a_game:
      return "a bundle is not launched itself; one of its games has to be "
             "chosen";
    default:
      break;
  }
  return "the PlayStation Plus and title checks could not be completed, so "
         "the launch was "
         "refused";
}

auto LaunchFailure::NotCleared::http_status() const noexcept -> std::uint16_t {
  const auto& cause = refusal_.cause();
  return cause ? cause->http_status() : std::uint16_t{};
}

auto LaunchFailure::NotCleared::refused() const -> LaunchRefused {
  return LaunchRefused::not_cleared(refusal_);
}

LaunchFailure::TitleRefused::TitleRefused(OldServiceLaunch::Error error)
    : error_(std::move(error)) {}

auto LaunchFailure::TitleRefused::describe() const noexcept
    -> std::string_view {
  using K = OldServiceLaunch::Error::Cause;
  switch (error_.cause()) {
    case K::cancelled:
      return "title preparation was cancelled";
    case K::invalid_request:
      return "the title request was invalid";
    case K::transport:
      return "the title service could not be reached";
    case K::invalid_response:
      return "the title service returned an invalid response";
    case K::rejected:
      return "the title service rejected the request";
    case K::not_found:
      return "Game not found: the product does not exist or is not available "
             "for cloud "
             "streaming";
    case K::no_entitlement:
      return "no streaming entitlement was found for this title";
    case K::privacy_required:
      return "PlayStation account privacy settings need to be completed";
    case K::subscription_required:
      return "the license preview was rejected; check your PlayStation Plus "
             "subscription";
    case K::not_free:
      return "Game not free: the title is no longer free; refresh the game "
             "list (no "
             "acquisition was submitted)";
    case K::outside_streaming_plan:
      return "the title's streaming licence is not part of any PlayStation "
             "Plus plan (no "
             "acquisition was submitted)";
    case K::store_region:
      return "this edition is sold in another PlayStation Store region than "
             "the account's";
  }
  return "the title service request failed";
}

auto LaunchFailure::TitleRefused::step() const noexcept -> std::string_view {
  using S = OldServiceLaunch::Error::Stage;
  switch (error_.stage()) {
    case S::anonymous_oauth:
      return "Title authorization";
    case S::anonymous_session:
      return "Title lookup session";
    case S::title:
      return "Store title resolution";
    case S::commerce_oauth:
      return "License authorization";
    case S::account_attributes:
      return "Account privacy validation";
    case S::entitlement:
      return "License ownership check";
    case S::checkout_preview:
      return "License acquisition preview";
    case S::checkout:
      return "Free license acquisition";
    case S::account_session:
      break;
  }
  return "LegacyCloudStream account session";
}

auto LaunchFailure::TitleRefused::refused() const -> LaunchRefused {
  using Why = OldServiceLaunch::Error::Cause;
  switch (error_.cause()) {
    case Why::not_found:
      return LaunchRefused::title_not_found();
    case Why::privacy_required:
      if (auto refused =
              LaunchRefused::privacy_settings(error_.privacy_upgrade_url()))
        return std::move(*refused);
      return LaunchRefused::privacy_settings({}).value_or(
          LaunchRefused::unavailable(error_.http_status()));
    case Why::subscription_required:
      return LaunchRefused::membership_rejected();
    case Why::outside_streaming_plan:
      return LaunchRefused::outside_streaming_plan();
    case Why::store_region:
      return LaunchRefused::store_region();
    case Why::not_free:
      if (auto refused =
              LaunchRefused::licence_not_free(error_.display_price()))
        return std::move(*refused);
      return LaunchRefused::licence_not_free({}).value_or(
          LaunchRefused::unavailable(error_.http_status()));
    default:
      break;
  }
  return LaunchRefused::unavailable(error_.http_status());
}

LaunchFailure::ReservationFailed::ReservationFailed(
    psn::gk::Error error, std::string_view forced_datacenter, bool stopped,
    std::optional<catalog::StreamRefusal::Kind> ps_refusal)
    : error_(std::move(error)),
      forced_datacenter_(forced_datacenter),
      stopped_(stopped),
      ps_refusal_(ps_refusal) {}

auto LaunchFailure::ReservationFailed::describe() const noexcept
    -> std::string_view {
  using Stage = psn::gk::Error::Stage;
  const auto detail = error_.detail();
  switch (error_.stage()) {
    case Stage::identity: {
      using K = psn::gk::Error::IdentityDetail;
      switch (static_cast<K>(detail)) {
        case K::cancelled:
          return "account authorization was cancelled";
        case K::random_failure:
          return "device identity generation failed";
        case K::invalid_request:
          return "the account authorization request was invalid";
        case K::rejected:
          return "the cloud sign-in check rejected the credentials";
        case K::transport_failure:
          return "the cloud sign-in check could not reach the server";
      }
      break;
    }
    case Stage::datacenters:
    case Stage::ping:
    case Stage::select: {
      using K = psn::gk::Error::DatacenterDetail;
      switch (static_cast<K>(detail)) {
        case K::cancelled:
          return "datacenter selection was cancelled";
        case K::fetch_failed:
          return "the datacenter list could not be retrieved";
        case K::measure_failed:
          return "the datacenter probes failed";
        case K::unavailable:
          return "the requested datacenter is unavailable";
        case K::rejected:
          return "no datacenter passed the latency and reachability checks";
        case K::select_failed:
          return "the datacenter selection request failed";
      }
      break;
    }
    case Stage::allocate: {
      using K = psn::gk::Error::AllocateDetail;
      switch (static_cast<K>(detail)) {
        case K::invalid_request:
          return "the allocation request was invalid";
        case K::cancelled:
          return "allocation was cancelled";
        case K::network_failed:
          return "allocation exhausted its network retries";
        case K::timed_out:
          return "the allocation deadline expired";
        case K::failed:
          return "the allocation response was rejected or invalid";
      }
      break;
    }
    default:
      break;
  }
  return {};
}

auto LaunchFailure::ReservationFailed::detail() const noexcept
    -> std::string_view {
  return error_.is_cancelled() || error_.is_invalid_request()
             ? std::string_view{}
             : error_.rejection();
}

auto LaunchFailure::ReservationFailed::http_status() const noexcept
    -> std::uint16_t {
  const auto status = error_.http_status();
  if (stopped_ || net::HttpStatus{status}.is_success() ||
      net::HttpStatus{status} == net::HttpStatus::found)
    return {};
  return status;
}

auto LaunchFailure::ReservationFailed::refused() const -> LaunchRefused {
  using Reason = psn::gk::Error::Reason;
  using Stage = psn::gk::Error::Stage;
  if (ps_refusal_) return LaunchRefused::refused_by_service(*ps_refusal_);
  if (error_.is_cancelled()) return LaunchRefused::unavailable();
  const auto best = error_.best_rtt_ms();
  switch (error_.reason()) {
    case Reason::psplus_rejected:
      return LaunchRefused::membership_rejected();
    case Reason::no_reachable_datacenter:
      return LaunchRefused::no_reachable_datacenter(
          best && *best > 0 ? std::optional{std::chrono::milliseconds{*best}}
                            : std::nullopt);
    case Reason::datacenter_unavailable:
      if (auto name = DatacenterName::parse(forced_datacenter_))
        return LaunchRefused::datacenter_unavailable(std::move(*name));
      return LaunchRefused::unavailable(http_status());
    case Reason::network_exhausted:
      return LaunchRefused::no_server(LaunchRefused::NoServer::network);
    case Reason::timed_out:
    case Reason::timed_out_unreachable:
      return LaunchRefused::no_server(LaunchRefused::NoServer::queue_timed_out);
    default:
      break;
  }
  if (error_.stage() == Stage::identity &&
      error_.reason() != Reason::transport &&
      error_.reason() != Reason::random_failure)
    return LaunchRefused::sign_in_expired();
  return LaunchRefused::unavailable(http_status());
}

auto LaunchFailure::precondition(Precondition failed) -> LaunchFailure {
  return LaunchFailure{Unmet{failed}};
}

auto LaunchFailure::not_cleared(const catalog::LaunchRefusal& refusal)
    -> LaunchFailure {
  using Kind = catalog::LaunchRefusal::Kind;
  if (refusal.kind() == Kind::unannounced ||
      refusal.kind() == Kind::not_released ||
      refusal.kind() == Kind::no_trusted_clock)
    return LaunchFailure{Unmet{Precondition::game_not_released, refusal}};
  return LaunchFailure{NotCleared{refusal}};
}

auto LaunchFailure::title(OldServiceLaunch::Error error) -> LaunchFailure {
  return LaunchFailure{TitleRefused{std::move(error)}};
}

auto LaunchFailure::reservation(psn::gk::Error error,
                                std::string_view forced_datacenter,
                                bool stopped) -> LaunchFailure {
  return LaunchFailure{
      ReservationFailed{std::move(error), forced_datacenter, stopped}};
}

auto LaunchFailure::refused_by_service(psn::gk::Error error,
                                       std::string_view forced_datacenter,
                                       catalog::StreamRefusal::Kind kind)
    -> LaunchFailure {
  return LaunchFailure{
      ReservationFailed{std::move(error), forced_datacenter, false, kind}};
}

auto LaunchFailure::describe() const noexcept -> std::string_view {
  return std::visit([](const auto& cause) { return cause.describe(); }, cause_);
}

auto LaunchFailure::detail() const noexcept -> std::string_view {
  const auto* reservation = as<ReservationFailed>();
  return reservation ? reservation->detail() : std::string_view{};
}

auto LaunchFailure::http_status() const noexcept -> std::uint16_t {
  if (const auto* refusal = as<NotCleared>()) return refusal->http_status();
  if (const auto* title = as<TitleRefused>())
    return title->error().http_status();
  if (const auto* reservation = as<ReservationFailed>())
    return reservation->http_status();
  return {};
}

auto LaunchFailure::upgrade_url() const noexcept -> std::string_view {
  const auto* title = as<TitleRefused>();
  return title ? title->error().privacy_upgrade_url() : std::string_view{};
}

auto LaunchFailure::best_rtt_ms() const noexcept -> std::optional<int> {
  const auto* reservation = as<ReservationFailed>();
  return reservation ? reservation->error().best_rtt_ms() : std::nullopt;
}

auto LaunchFailure::refused() const -> LaunchRefused {
  return std::visit([](const auto& cause) { return cause.refused(); }, cause_);
}

}  // namespace libchiaking::play
