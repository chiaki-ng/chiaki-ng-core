#include "src/core/play/adapters/links/internet/route_failure.hpp"

#include "src/core/play/adapters/links/internet/psn_failure.hpp"

namespace libchiaking::play {

auto RouteFailure::of(
    const internet::holepunch::CtrlAcceptTransportError& error) noexcept
    -> RouteFailure {
  if (const auto handoff = error.handoff()) return RouteFailure{*handoff};
  return RouteFailure{*error.exchange()};
}

auto RouteFailure::describe() const noexcept -> std::string_view {
  return std::visit([](const auto& cause) { return describe(cause); }, cause_);
}

auto RouteFailure::cancelled() const noexcept -> bool {
  using Exchange = internet::holepunch::SelectedAcceptExchangeErrorKind;
  using Wait = internet::holepunch::AcceptPsnErrorKind;
  if (const auto* offer = as<internet::holepunch::OfferPsnError>()) {
    if (offer->kind() == internet::holepunch::OfferPsnErrorKind::push_failure)
      return offer->push() && PsnFailure::cancelled(offer->push()->push);
    const auto send = offer->send();
    return offer->kind() ==
               internet::holepunch::OfferPsnErrorKind::send_failure &&
           send && send->session() && PsnFailure::cancelled(*send->session());
  }
  const auto* exchange = as<internet::holepunch::SelectedAcceptExchangeError>();
  if (!exchange) return false;
  switch (exchange->kind()) {
    case Exchange::late_probe_failed:
      return exchange->probe() == net::nat::holepunch::ProbeWaitError::cancelled;
    case Exchange::wait_failed: {
      const auto wait = *exchange->wait();
      if (wait.kind() == Wait::push_failure)
        return wait.push() && PsnFailure::cancelled(wait.push()->push);
      return wait.kind() == Wait::cancelled;
    }
    case Exchange::build_failed:
    case Exchange::send_failed:
      break;
  }
  return false;
}

auto RouteFailure::http_status() const noexcept -> std::uint16_t {
  using Exchange = internet::holepunch::SelectedAcceptExchangeErrorKind;
  using Wait = internet::holepunch::AcceptPsnErrorKind;
  if (const auto* offer = as<internet::holepunch::OfferPsnError>()) {
    if (offer->kind() == internet::holepunch::OfferPsnErrorKind::push_failure)
      return offer->push() ? PsnFailure::http_status(offer->push()->push)
                           : std::uint16_t{};
    const auto send = offer->send();
    if (offer->kind() == internet::holepunch::OfferPsnErrorKind::send_failure &&
        send && send->session())
      return send->session()->status;
    return {};
  }
  const auto* exchange = as<internet::holepunch::SelectedAcceptExchangeError>();
  if (!exchange) return {};
  switch (exchange->kind()) {
    case Exchange::send_failed: {
      const auto session = exchange->send()->session();
      return session ? session->status : std::uint16_t{};
    }
    case Exchange::wait_failed: {
      const auto wait = *exchange->wait();
      if (wait.kind() == Wait::push_failure)
        return wait.push() ? PsnFailure::http_status(wait.push()->push)
                           : std::uint16_t{};
      const auto send = wait.send();
      if (wait.kind() == Wait::send_failure && send && send->session())
        return send->session()->status;
      return {};
    }
    case Exchange::build_failed:
    case Exchange::late_probe_failed:
      break;
  }
  return {};
}

auto RouteFailure::drains_late_probe() const noexcept -> bool {
  const auto* exchange = as<internet::holepunch::SelectedAcceptExchangeError>();
  return exchange && exchange->kind() ==
                         internet::holepunch::SelectedAcceptExchangeErrorKind::
                             late_probe_failed;
}

auto RouteFailure::describe(InvalidState) noexcept -> std::string_view {
  return "the route owner was in an invalid state";
}

auto RouteFailure::describe(Cancelled) noexcept -> std::string_view {
  return "cancelled";
}

auto RouteFailure::describe(TimedOut) noexcept -> std::string_view {
  return "the route deadline expired";
}

auto RouteFailure::describe(NoHttpProvider) noexcept -> std::string_view {
  return "the HTTP provider is unavailable";
}

auto RouteFailure::describe(PollerUnavailable) noexcept -> std::string_view {
  return "the network poller could not be created";
}

auto RouteFailure::describe(
    net::nat::holepunch::BoundSocketsError error) noexcept -> std::string_view {
  using E = net::nat::holepunch::BoundSocketsError;
  switch (error) {
    case E::ipv4_create:
      return "the local UDP socket could not be created";
    case E::ipv4_shared_port:
      return "shared UDP port setup failed";
    case E::ipv4_bind:
      return "the local UDP socket could not be bound";
    case E::ipv4_endpoint:
      return "the bound UDP endpoint was invalid";
  }
  return "local UDP socket setup failed";
}

auto RouteFailure::describe(net::nat::holepunch::GatherError) noexcept
    -> std::string_view {
  return "candidate address gathering failed";
}

auto RouteFailure::describe(net::nat::holepunch::ConnRequestError) noexcept
    -> std::string_view {
  return "the hole-punch offer could not be prepared";
}

auto RouteFailure::describe(
    const internet::holepunch::OfferPsnError& error) noexcept
    -> std::string_view {
  if (error.kind() == internet::holepunch::OfferPsnErrorKind::push_failure &&
      error.push())
    return PsnFailure::describe(error.push()->push);
  const auto send = error.send();
  if (error.kind() == internet::holepunch::OfferPsnErrorKind::send_failure &&
      send && send->session())
    return PsnFailure::describe(*send->session());
  return "PSN offer exchange failed";
}

auto RouteFailure::describe(internet::holepunch::OfferProbeError) noexcept
    -> std::string_view {
  return "peer connectivity probing failed";
}

auto RouteFailure::describe(
    const internet::holepunch::SelectedAcceptExchangeError& error) noexcept
    -> std::string_view {
  using K = internet::holepunch::SelectedAcceptExchangeErrorKind;
  switch (error.kind()) {
    case K::late_probe_failed:
      return describe(*error.probe());
    case K::build_failed:
      return describe(*error.build());
    case K::send_failed:
      return "sending the selected route ACCEPT request through PSN failed";
    case K::wait_failed: {
      const auto wait = *error.wait();
      if (wait.kind() ==
              internet::holepunch::AcceptPsnErrorKind::push_failure &&
          wait.push())
        return PsnFailure::describe(wait.push()->push);
      return describe(wait.kind());
    }
  }
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(
    internet::holepunch::CtrlRudpHandoffError error) noexcept
    -> std::string_view {
  using K = internet::holepunch::CtrlRudpHandoffError;
  switch (error) {
    case K::accept_not_ready:
      return "the control socket handoff began before ACCEPT completed";
    case K::null_socket:
      return "the accepted control socket was missing";
    case K::foreign_provider:
      return "the accepted control socket belonged to another provider";
    case K::invalid_peer:
      return "the accepted control endpoint was invalid";
    case K::unbound_socket:
      return "the accepted control socket was not bound";
    case K::adoption_failed:
      return "adopting the accepted control socket into RUDP failed";
  }
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(AcceptNotReady) noexcept -> std::string_view {
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(Adoption) noexcept -> std::string_view {
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(
    internet::holepunch::AcceptPsnErrorKind kind) noexcept -> std::string_view {
  using K = internet::holepunch::AcceptPsnErrorKind;
  switch (kind) {
    case K::invalid_attempt:
      return "the ACCEPT wait attempt was invalid";
    case K::cancelled:
      return "waiting for the console ACCEPT was cancelled";
    case K::timed_out:
      return "timed out waiting for the console ACCEPT";
    case K::push_failure:
      return "the PSN notification connection failed while waiting for "
             "ACCEPT";
    case K::invalid_message:
      return "a PSN message could not be decoded while waiting for ACCEPT";
    case K::send_failure:
      return "acknowledging the console ACCEPT through PSN failed";
    case K::terminated:
      return "the console terminated the route before ACCEPT";
  }
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(
    internet::holepunch::SelectedAcceptError error) noexcept
    -> std::string_view {
  using K = internet::holepunch::SelectedAcceptError;
  switch (error) {
    case K::candidate_index_out_of_range:
      return "ACCEPT preparation failed: selected candidate index is invalid";
    case K::malformed_local_offer:
      return "ACCEPT preparation failed: local offer candidate layout is "
             "invalid";
    case K::sid_out_of_range:
      return "ACCEPT preparation failed: session identifier is out of range";
  }
  return "the selected route could not be accepted";
}

auto RouteFailure::describe(net::nat::holepunch::ProbeWaitError error) noexcept
    -> std::string_view {
  using K = net::nat::holepunch::ProbeWaitError;
  switch (error) {
    case K::cancelled:
      return "post-ACCEPT probe drain was interrupted";
    case K::timeout:
      return "post-ACCEPT probe drain timed out";
    case K::net:
      return "post-ACCEPT probe socket failed";
    case K::protocol:
      return "post-ACCEPT probe packet was invalid";
  }
  return "the selected route could not be accepted";
}

}  // namespace libchiaking::play
