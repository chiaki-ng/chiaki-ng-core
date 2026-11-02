#include "src/core/play/adapters/links/internet/psn_failure.hpp"

namespace libchiaking::play {

auto PsnFailure::describe() const noexcept -> std::string_view {
  return std::visit([](const auto& cause) { return describe(cause); }, cause_);
}

auto PsnFailure::cancelled() const noexcept -> bool {
  return std::visit([](const auto& cause) { return cancelled(cause); }, cause_);
}

auto PsnFailure::http_status() const noexcept -> std::uint16_t {
  return std::visit([](const auto& cause) { return http_status(cause); },
                    cause_);
}

auto PsnFailure::lacks_access() const noexcept -> bool {
  using E = psn::remote_play::StartError;
  const auto* open = as<E>();
  const auto* push = open ? open->as<E::PushOpenFailed>() : nullptr;
  return push && push->push_open.kind() ==
                     psn::remote_play::PushOpenError::Kind::invalid_token;
}

auto PsnFailure::offline() const noexcept -> bool {
  const auto* start = as<psn::remote_play::StartError>();
  const auto inner = start ? unwrapped(*start) : std::optional<Cause>{cause_};
  const auto* request =
      inner ? std::get_if<psn::remote_play::SessionError>(&*inner) : nullptr;
  return request &&
         request->kind ==
             psn::remote_play::SessionErrorKind::transport_failure &&
         request->transport == platform::HttpError::disconnected;
}

auto PsnFailure::describe(const psn::remote_play::SessionError& error) noexcept
    -> std::string_view {
  using K = psn::remote_play::SessionErrorKind;
  switch (error.kind) {
    case K::invalid_token:
      return "the PSN credential is invalid";
    case K::cancelled:
      return "the PSN request was interrupted";
    case K::random_failure:
      return "secure random generation failed";
    case K::transport_failure:
      return "the PSN HTTP transport failed";
    case K::http_status:
      return "PSN rejected the request";
    case K::invalid_response:
      return "the PSN response was invalid";
    case K::invalid_request:
      return "the PSN request could not be encoded";
  }
  return "the PSN request failed";
}

auto PsnFailure::describe(const psn::remote_play::PushError& error) noexcept
    -> std::string_view {
  if (error.acknowledgement) return describe(*error.acknowledgement);
  using K = psn::remote_play::PushErrorKind;
  switch (error.kind) {
    case K::invalid_token:
      return "the PSN push credential is invalid";
    case K::invalid_fqdn:
      return "the PSN push server address is invalid";
    case K::cancelled:
      return "the PSN push operation was interrupted";
    case K::websocket_failure:
      switch (error.backend) {
        case platform::WebSocketError::timed_out:
          return "the PSN notification WebSocket timed out";
        case platform::WebSocketError::disconnected:
          return "the PSN notification WebSocket disconnected";
        case platform::WebSocketError::protocol_error:
          return "the PSN notification WebSocket received an invalid frame";
        case platform::WebSocketError::message_too_large:
          return "the PSN notification WebSocket exceeded its message or "
                 "queue limit";
        default:
          return "the PSN notification connection failed";
      }
    case K::invalid_notification:
      return "a PSN notification was invalid";
    case K::notification_queue_full:
      return "the PSN notification queue filled";
    case K::session_deleted:
      return "PSN deleted the active remote session";
    case K::session_terminated:
      return "the console terminated PSN signalling";
    case K::acknowledgement_failed:
      return "acknowledging the console OFFER failed";
    case K::connection_mismatch:
      return "a notification belonged to a different PSN connection";
  }
  return "the PSN notification connection failed";
}

auto PsnFailure::describe(const Wait& wait) noexcept -> std::string_view {
  if (wait.push) return describe(*wait.push);
  using K = psn::remote_play::CreationWaitError;
  switch (wait.error) {
    case K::stale_attempt:
      return "a stale PSN attempt was rejected";
    case K::timed_out:
      return "the expected PSN notification did not arrive before the "
             "deadline";
    case K::cancelled:
      return "the PSN notification wait was interrupted";
    case K::receive_failed:
      return "receiving a PSN notification failed";
    case K::invalid_notification:
      return "the PSN state machine rejected a notification";
    case K::too_many_notifications:
      return "the PSN notification limit was reached";
    case K::failed:
      return "the PSN notification state machine failed";
  }
  return "the PSN notification wait failed";
}

auto PsnFailure::describe(const psn::remote_play::StartError& error) noexcept
    -> std::string_view {
  using E = psn::remote_play::StartError;
  if (const auto nested = unwrapped(error))
    return std::visit([](const auto& cause) { return describe(cause); },
                      *nested);
  switch (error.kind()) {
    case E::Kind::invalid_attempt:
      return "the PSN creation attempt was invalid";
    case E::Kind::timed_out:
      return "the PSN creation deadline expired";
    case E::Kind::cancelled:
      return "the PSN creation operation was cancelled";
    case E::Kind::push_open_failed:
    case E::Kind::create_failed:
    case E::Kind::wait_failed:
      break;
  }
  return "the PSN creation operation could not start";
}

auto PsnFailure::unwrapped(const psn::remote_play::StartError& error) noexcept
    -> std::optional<Cause> {
  using E = psn::remote_play::StartError;
  using P = psn::remote_play::PushOpenError;
  if (const auto* create = error.as<E::CreateFailed>())
    return Cause{create->create};
  if (const auto* wait = error.as<E::WaitFailed>())
    return Cause{Wait{wait->wait, wait->push}};
  if (const auto* push_open = error.as<E::PushOpenFailed>()) {
    if (const auto* fqdn = push_open->push_open.as<P::GetFqdnFailed>())
      return Cause{fqdn->session};
    if (const auto* connect = push_open->push_open.as<P::ConnectFailed>())
      return Cause{connect->push};
  }
  return std::nullopt;
}

auto PsnFailure::cancelled(const psn::remote_play::SessionError& error) noexcept
    -> bool {
  return error.kind == psn::remote_play::SessionErrorKind::cancelled;
}

auto PsnFailure::cancelled(const psn::remote_play::PushError& error) noexcept
    -> bool {
  if (error.acknowledgement) return cancelled(*error.acknowledgement);
  return error.kind == psn::remote_play::PushErrorKind::cancelled;
}

auto PsnFailure::cancelled(const Wait& wait) noexcept -> bool {
  if (wait.push) return cancelled(*wait.push);
  return wait.error == psn::remote_play::CreationWaitError::cancelled;
}

auto PsnFailure::cancelled(const psn::remote_play::StartError& error) noexcept
    -> bool {
  if (const auto nested = unwrapped(error))
    return std::visit([](const auto& cause) { return cancelled(cause); },
                      *nested);
  return error.kind() == psn::remote_play::StartError::Kind::cancelled;
}

auto PsnFailure::http_status(
    const psn::remote_play::SessionError& error) noexcept -> std::uint16_t {
  return error.status;
}

auto PsnFailure::http_status(const psn::remote_play::PushError& error) noexcept
    -> std::uint16_t {
  return error.acknowledgement ? error.acknowledgement->status
                               : std::uint16_t{};
}

auto PsnFailure::http_status(const Wait& wait) noexcept -> std::uint16_t {
  return wait.push ? http_status(*wait.push) : std::uint16_t{};
}

auto PsnFailure::http_status(const psn::remote_play::StartError& error) noexcept
    -> std::uint16_t {
  if (const auto nested = unwrapped(error))
    return std::visit([](const auto& cause) { return http_status(cause); },
                      *nested);
  return {};
}

}  // namespace libchiaking::play
