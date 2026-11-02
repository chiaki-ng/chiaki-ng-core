#include "src/core/psn/gk/error.hpp"

#include <utility>

namespace libchiaking::psn::gk {

auto Error::reason_of(const psn::auth::AuthError& error) noexcept -> Reason {
  switch (error.kind()) {
    case psn::auth::AuthErrorKind::cancelled:
      return Reason::cancelled;
    case psn::auth::AuthErrorKind::invalid_request:
      return Reason::invalid_authorization;
    case psn::auth::AuthErrorKind::invalid_npsso:
      return Reason::invalid_npsso;
    default:
      return error.denied() ? Reason::rejected : Reason::transport;
  }
}

Error::Error(Stage stage, Reason reason, std::uint16_t http_status) noexcept
    : stage_(stage), reason_(reason), http_status_(http_status) {}

Error::Error(Stage stage, const net::http::Failure& failure) noexcept
    : stage_(stage),
      reason_(failure.is_cancelled() ? Reason::cancelled : Reason::transport) {}

Error::Error(Stage stage, const psn::auth::AuthError& error) noexcept
    : stage_(stage),
      reason_(reason_of(error)),
      http_status_(error.http_status()) {}

auto Error::rejected(Stage stage, Reason reason, std::uint16_t http_status,
                     std::string_view body) -> Error {
  Error error{stage, reason, http_status};
  error.rejection_ = body;
  return error;
}

auto Error::no_reachable_datacenter(std::optional<int> best_rtt_ms) noexcept
    -> Error {
  Error error{Stage::select, Reason::no_reachable_datacenter};
  error.best_rtt_ms_ = best_rtt_ms;
  return error;
}

auto Error::is_transport() const noexcept -> bool {
  return reason_ == Reason::transport || reason_ == Reason::network_exhausted ||
         reason_ == Reason::timed_out_unreachable;
}

auto Error::is_invalid_request() const noexcept -> bool {
  switch (reason_) {
    case Reason::invalid_service:
    case Reason::invalid_session_id:
    case Reason::invalid_config_key:
    case Reason::invalid_spec:
    case Reason::invalid_ping_results:
    case Reason::invalid_allocate_body:
    case Reason::body_too_large:
    case Reason::invalid_npsso:
    case Reason::invalid_authorization:
      return true;
    default:
      return false;
  }
}

auto Error::detail() const noexcept -> std::uint32_t {
  const bool cancelled = is_cancelled();
  const bool invalid = is_invalid_request();
  switch (stage_) {
    case Stage::identity:
      if (cancelled) return code(IdentityDetail::cancelled);
      if (reason_ == Reason::random_failure)
        return code(IdentityDetail::random_failure);
      if (invalid) return code(IdentityDetail::invalid_request);
      return code(reason_ == Reason::transport
                      ? IdentityDetail::transport_failure
                      : IdentityDetail::rejected);
    case Stage::client_ids:
    case Stage::config:
      if (cancelled) return code(ServiceDetail::cancelled);
      if (reason_ == Reason::transport)
        return code(ServiceDetail::transport_failure);
      if (invalid) return code(ServiceDetail::invalid_request);
      return code(reason_ == Reason::rejected ? ServiceDetail::rejected
                                              : ServiceDetail::malformed);
    case Stage::start:
      if (cancelled) return code(StartDetail::cancelled);
      if (invalid) return code(StartDetail::invalid_request);
      return code(reason_ == Reason::entitlement_retry
                      ? StartDetail::entitlement_retry
                      : StartDetail::rejected);
    case Stage::oauth:
      if (cancelled) return code(OAuthDetail::cancelled);
      if (reason_ == Reason::invalid_authorization)
        return code(OAuthDetail::invalid_authorization);
      if (reason_ == Reason::invalid_npsso)
        return code(OAuthDetail::invalid_npsso);
      return code(invalid ? OAuthDetail::invalid_request
                          : OAuthDetail::rejected);
    case Stage::authorize:
      if (invalid) return code(AuthorizeDetail::invalid_request);
      if (cancelled) return code(AuthorizeDetail::cancelled);
      if (reason_ == Reason::psplus_rejected)
        return code(AuthorizeDetail::psplus_rejected);
      return code(reason_ == Reason::entitlement_retry
                      ? AuthorizeDetail::entitlement_retry
                      : AuthorizeDetail::rejected);
    case Stage::lock:
      if (invalid) return code(LockDetail::invalid_request);
      return code(cancelled ? LockDetail::cancelled : LockDetail::unavailable);
    case Stage::datacenters:
      return code(cancelled ? DatacenterDetail::cancelled
                            : DatacenterDetail::fetch_failed);
    case Stage::ping:
      return code(cancelled ? DatacenterDetail::cancelled
                            : DatacenterDetail::measure_failed);
    case Stage::select:
      if (cancelled) return code(DatacenterDetail::cancelled);
      if (reason_ == Reason::datacenter_unavailable)
        return code(DatacenterDetail::unavailable);
      return code(reason_ == Reason::no_reachable_datacenter
                      ? DatacenterDetail::rejected
                      : DatacenterDetail::select_failed);
    case Stage::allocate:
      if (invalid) return code(AllocateDetail::invalid_request);
      if (cancelled) return code(AllocateDetail::cancelled);
      if (reason_ == Reason::network_exhausted)
        return code(AllocateDetail::network_failed);
      return code(reason_ == Reason::timed_out ||
                          reason_ == Reason::timed_out_unreachable
                      ? AllocateDetail::timed_out
                      : AllocateDetail::failed);
  }
  std::unreachable();
}

}  // namespace libchiaking::psn::gk
