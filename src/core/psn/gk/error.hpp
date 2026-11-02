/// @file
/// @brief Declares gk::Error, the failure of a step of a GK cloud
/// launch, with the stage it happened at and the numeric detail code launch
/// failures are reported with.
#pragma once

#include "src/core/net/transport/http/failure.hpp"
#include "src/core/psn/auth/auth_error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::psn::gk {

/// @brief A cloud launch step's failure: which stage, why, the HTTP status, and
/// for some rejections GK's reply body. An immutable value; safe to copy
/// and share.
class Error {
 public:
  /// @brief The launch stage that failed, in launch order.
  enum class Stage : std::uint8_t {
    /// @brief Checking the device and user: the authorizeCheck, before any
    /// GK request.
    identity,
    /// @brief Asking GK for the session's OAuth client ids.
    client_ids,
    /// @brief Asking GK for a configuration key.
    config,
    /// @brief Starting a GK session.
    start,
    /// @brief Asking the authorisation server for the GK codes.
    oauth,
    /// @brief Authorising the session with the codes.
    authorize,
    /// @brief Locking the account to this session, logging out any other.
    lock,
    /// @brief Listing the datacenters.
    datacenters,
    /// @brief Measuring the datacenters.
    ping,
    /// @brief Telling GK the measurements and choosing a datacenter.
    select,
    /// @brief Asking for a cloud console, and waiting in the queue.
    allocate,
  };

  /// @brief Why the stage failed.
  enum class Reason : std::uint8_t {
    /// @brief The stop token fired.
    cancelled,
    /// @brief A request did not complete.
    transport,
    /// @brief The service does not support the platform; nothing was sent.
    invalid_service,
    /// @brief The session id is empty or holds NUL, CR or LF; nothing was
    /// sent.
    invalid_session_id,
    /// @brief The configuration key is empty or holds NUL, CR or LF;
    /// nothing was sent.
    invalid_config_key,
    /// @brief The launch specification is not JSON, names an unknown user
    /// agent, or cannot be canonicalised; nothing was sent.
    invalid_spec,
    /// @brief The measurements could not be written as a report; nothing
    /// was sent.
    invalid_ping_results,
    /// @brief The selected datacenter's name holds a NUL; nothing was sent.
    invalid_allocate_body,
    /// @brief A body would exceed the document size limit; nothing was
    /// sent.
    body_too_large,
    /// @brief The user has no usable NPSSO.
    invalid_npsso,
    /// @brief An authorisation request could not be built.
    invalid_authorization,
    /// @brief GK or the authorisation server answered no.
    rejected,
    /// @brief GK answered that it found no game for the entitlement
    /// (`noGameForEntitlement`); the launch may be retried.
    entitlement_retry,
    /// @brief GK refused to authorise for want of PlayStation Plus
    /// (event or body names `002.2001`).
    psplus_rejected,
    /// @brief GK rotated the configuration key to a value that cannot
    /// be sent.
    invalid_session_key,
    /// @brief A GK reply lacked what the stage needs.
    malformed,
    /// @brief The allocation reply was neither progress nor a launch slot.
    missing_launch_slot,
    /// @brief The account lock could not be acquired in the attempts
    /// allowed.
    lock_unavailable,
    /// @brief The datacenters could not be measured.
    probe_failed,
    /// @brief The datacenter the user forced is not offered.
    datacenter_unavailable,
    /// @brief No datacenter was reachable fast enough.
    no_reachable_datacenter,
    /// @brief Allocation polling met too many network failures.
    network_exhausted,
    /// @brief The allocation queue did not finish within its wait limit.
    timed_out,
    /// @brief The allocation timed out without GK ever answering.
    timed_out_unreachable,
    /// @brief Random bytes could not be drawn; not produced at present.
    random_failure,
  };

  /// @brief Detail codes of the identity stage.
  enum class IdentityDetail : std::uint8_t {
    /// @brief Cancelled.
    cancelled,
    /// @brief Random bytes could not be drawn.
    random_failure,
    /// @brief The request could not be built.
    invalid_request,
    /// @brief The authorisation server said no.
    rejected,
    /// @brief The request did not complete.
    transport_failure,
  };

  /// @brief Detail codes of the client ids and config stages.
  enum class ServiceDetail : std::uint8_t {
    /// @brief The request could not be built.
    invalid_request,
    /// @brief Cancelled.
    cancelled,
    /// @brief The request did not complete.
    transport_failure,
    /// @brief GK said no.
    rejected,
    /// @brief GK's reply was unreadable.
    malformed,
  };

  /// @brief Detail codes of the start stage.
  enum class StartDetail : std::uint8_t {
    /// @brief Cancelled.
    cancelled,
    /// @brief No game for the entitlement.
    entitlement_retry,
    /// @brief The request could not be built.
    invalid_request,
    /// @brief GK said no, or the request failed otherwise.
    rejected,
  };

  /// @brief Detail codes of the oauth stage.
  enum class OAuthDetail : std::uint8_t {
    /// @brief Cancelled.
    cancelled,
    /// @brief The authorisation request could not be built.
    invalid_authorization,
    /// @brief No usable NPSSO.
    invalid_npsso,
    /// @brief The authorisation server said no, or could not be reached.
    rejected,
    /// @brief Another request that could not be built.
    invalid_request,
  };

  /// @brief Detail codes of the authorize stage.
  enum class AuthorizeDetail : std::uint8_t {
    /// @brief The request could not be built.
    invalid_request,
    /// @brief Cancelled.
    cancelled,
    /// @brief PlayStation Plus is required.
    psplus_rejected,
    /// @brief No game for the entitlement.
    entitlement_retry,
    /// @brief GK said no, or the request failed otherwise.
    rejected,
  };

  /// @brief Detail codes of the lock stage; 2 to 4 are unused.
  enum class LockDetail : std::uint8_t {
    /// @brief The request could not be built.
    invalid_request,
    /// @brief Cancelled.
    cancelled,
    /// @brief The lock could not be taken.
    unavailable = 5,
  };

  /// @brief Detail codes of the datacenters, ping and select stages.
  enum class DatacenterDetail : std::uint8_t {
    /// @brief Cancelled.
    cancelled,
    /// @brief The list could not be fetched.
    fetch_failed,
    /// @brief The datacenters could not be measured.
    measure_failed,
    /// @brief The forced datacenter is not offered.
    unavailable,
    /// @brief No datacenter was reachable fast enough.
    rejected,
    /// @brief The selection failed otherwise.
    select_failed,
  };

  /// @brief Detail codes of the allocate stage.
  enum class AllocateDetail : std::uint8_t {
    /// @brief The request could not be built.
    invalid_request,
    /// @brief Cancelled.
    cancelled,
    /// @brief Too many network failures while polling.
    network_failed,
    /// @brief The queue did not finish in time.
    timed_out,
    /// @brief The allocation failed otherwise.
    failed,
  };

  /// @brief Makes a failure.
  ///
  /// @param stage The stage.
  /// @param reason Why.
  /// @param http_status The reply's status; 0 when none.
  Error(Stage stage, Reason reason, std::uint16_t http_status = 0) noexcept;
  /// @brief Wraps a request that did not complete.
  ///
  /// @param stage The stage.
  /// @param failure The client's failure: cancelled or transport.
  Error(Stage stage, const net::http::Failure& failure) noexcept;
  /// @brief Wraps an authorisation failure.
  ///
  /// @param stage The stage.
  /// @param error The failure: cancelled; invalid_authorization for an
  ///     invalid request; invalid_npsso; rejected when the server said no
  ///     (AuthError::denied()); transport otherwise. Its status is kept.
  Error(Stage stage, const psn::auth::AuthError& error) noexcept;

  /// @brief Makes a rejection that keeps GK's reply body.
  ///
  /// @param stage The stage.
  /// @param reason Why.
  /// @param http_status The reply's status.
  /// @param body The reply's body, kept for the failure report.
  /// @return The failure.
  static auto rejected(Stage stage, Reason reason, std::uint16_t http_status,
                       std::string_view body) -> Error;
  /// @brief Makes the select failure for no reachable datacenter.
  ///
  /// @param best_rtt_ms The best round-trip time measured, in ms, if any.
  /// @return The failure.
  static auto no_reachable_datacenter(std::optional<int> best_rtt_ms) noexcept
      -> Error;

  /// @brief Reads the stage.
  ///
  /// @return The stage.
  auto stage() const noexcept -> Stage { return stage_; }
  /// @brief Reads why.
  ///
  /// @return The reason.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Reads the reply's status.
  ///
  /// @return It, or 0.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief Tells whether the launch was cancelled.
  ///
  /// @return True for cancelled.
  auto is_cancelled() const noexcept -> bool {
    return reason_ == Reason::cancelled;
  }
  /// @brief Tells whether the network failed.
  ///
  /// @return True for transport, network_exhausted and timed_out_unreachable.
  auto is_transport() const noexcept -> bool;
  /// @brief Tells whether the library refused to build a request.
  ///
  /// @return True for invalid_service, invalid_session_id,
  ///     invalid_config_key, invalid_spec, invalid_ping_results,
  ///     invalid_allocate_body, body_too_large, invalid_npsso and
  ///     invalid_authorization.
  auto is_invalid_request() const noexcept -> bool;
  /// @brief Gives the stage's numeric detail code for the failure report.
  ///
  /// @return The value of the stage's detail enumerator the reason maps to.
  auto detail() const noexcept -> std::uint32_t;
  /// @brief Reads GK's reply body for a rejection.
  ///
  /// @return The body, or empty; valid while this object lives.
  auto rejection() const noexcept -> std::string_view { return rejection_; }
  /// @brief Reads the best round-trip time when no datacenter was reachable.
  ///
  /// @return It, in ms, or nothing.
  auto best_rtt_ms() const noexcept -> std::optional<int> {
    return best_rtt_ms_;
  }

 private:
  /// @brief Turns a detail enumerator into its code.
  ///
  /// @tparam Detail A detail enumeration.
  /// @param detail The enumerator.
  /// @return Its underlying value.
  template <class Detail>
  static constexpr auto code(Detail detail) noexcept -> std::uint32_t {
    return std::to_underlying(detail);
  }
  /// @brief Classifies an authorisation failure.
  ///
  /// @param error The failure.
  /// @return The reason the AuthError constructor describes.
  static auto reason_of(const psn::auth::AuthError& error) noexcept -> Reason;

  /// @brief The stage.
  Stage stage_;
  /// @brief Why.
  Reason reason_;
  /// @brief The reply's status; 0 when none.
  std::uint16_t http_status_{};
  /// @brief GK's reply body for a rejection.
  std::string rejection_;
  /// @brief The best round-trip time, for no_reachable_datacenter.
  std::optional<int> best_rtt_ms_;
};

}  // namespace libchiaking::psn::gk
