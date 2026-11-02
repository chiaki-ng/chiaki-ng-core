/// @file
/// @brief Declares RouteFailure, why an internet start could not open its
/// route to the console: local sockets, candidates, the PSN offer, the
/// accept exchange or the socket handoff.
#pragma once

#include "libchiaking/platform/net/types.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace libchiaking::play {

/// @brief A failure to open the hole-punched route to a console. A value.
class RouteFailure {
 public:
  /// @brief The route owner was in a state that does not allow the step.
  struct InvalidState {};
  /// @brief The start was stopped.
  struct Cancelled {};
  /// @brief The start's deadline passed.
  struct TimedOut {};
  /// @brief The platform has no HTTP provider.
  struct NoHttpProvider {};
  /// @brief No network poller could be made.
  struct PollerUnavailable {
    /// @brief The network error.
    platform::NetError error{platform::NetError::not_supported};
  };
  /// @brief The handoff began before the console's ACCEPT completed.
  struct AcceptNotReady {};

  /// @brief Why an accepted data socket could not become the stream route.
  enum class Adoption : std::uint8_t {
    /// @brief The socket was missing.
    null_socket,
    /// @brief The socket belongs to another network provider.
    foreign_provider,
    /// @brief The selected peer address is unusable or of another family.
    invalid_peer,
    /// @brief The socket is not bound.
    unbound_socket,
    /// @brief Setting the socket up or wrapping it failed.
    transport_error,
  };

  /// @brief The failure of whichever route step failed.
  using Cause = std::variant<
      InvalidState, Cancelled, TimedOut, NoHttpProvider, PollerUnavailable,
      net::nat::holepunch::BoundSocketsError, net::nat::holepunch::GatherError,
      net::nat::holepunch::ConnRequestError, internet::holepunch::OfferPsnError,
      internet::holepunch::OfferProbeError,
      internet::holepunch::SelectedAcceptExchangeError,
      internet::holepunch::CtrlRudpHandoffError, AcceptNotReady, Adoption>;

  /// @brief Builds the failure.
  ///
  /// @param cause The cause.
  RouteFailure(Cause cause) noexcept : cause_(std::move(cause)) {}
  /// @brief Turns a control accept transport failure into a route failure.
  ///
  /// @param error The failure.
  /// @return The handoff's error, or the exchange's.
  static auto of(
      const internet::holepunch::CtrlAcceptTransportError& error) noexcept
      -> RouteFailure;

  /// @brief Reports whether the cause is one alternative.
  ///
  /// @tparam T One of the Cause alternatives.
  /// @return `true` when it is.
  template <class T>
  auto is() const noexcept -> bool {
    return std::holds_alternative<T>(cause_);
  }
  /// @brief Reads the cause as one alternative.
  ///
  /// @tparam T One of the Cause alternatives.
  /// @return The cause when it is a @p T; null otherwise.
  template <class T>
  auto as() const noexcept -> const T* {
    return std::get_if<T>(&cause_);
  }
  /// @brief Describes the failure.
  ///
  /// @return A fixed sentence for the cause.
  auto describe() const noexcept -> std::string_view;
  /// @brief Reports whether the failure was a cancellation.
  ///
  /// @return `true` when the PSN offer, the accept wait or the late probe
  ///     drain was cancelled.
  auto cancelled() const noexcept -> bool;
  /// @brief Gives the HTTP status behind the failure.
  ///
  /// @return The PSN request's status; 0 otherwise.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief Reports whether the failure happened while draining late probes
  /// after ACCEPT.
  ///
  /// @return `true` for a late probe failure.
  auto drains_late_probe() const noexcept -> bool;

 private:
  /// @brief Describes an invalid state.
  ///
  /// @return A fixed sentence.
  static auto describe(InvalidState) noexcept -> std::string_view;
  /// @brief Describes a cancellation.
  ///
  /// @return "cancelled".
  static auto describe(Cancelled) noexcept -> std::string_view;
  /// @brief Describes a passed deadline.
  ///
  /// @return A fixed sentence.
  static auto describe(TimedOut) noexcept -> std::string_view;
  /// @brief Describes a missing HTTP provider.
  ///
  /// @return A fixed sentence.
  static auto describe(NoHttpProvider) noexcept -> std::string_view;
  /// @brief Describes a missing poller.
  ///
  /// @return A fixed sentence.
  static auto describe(PollerUnavailable) noexcept -> std::string_view;
  /// @brief Describes a local socket failure.
  ///
  /// @param error The failure.
  /// @return A fixed sentence for its step.
  static auto describe(net::nat::holepunch::BoundSocketsError error) noexcept
      -> std::string_view;
  /// @brief Describes a candidate gathering failure.
  ///
  /// @return A fixed sentence.
  static auto describe(net::nat::holepunch::GatherError) noexcept
      -> std::string_view;
  /// @brief Describes an offer preparation failure.
  ///
  /// @return A fixed sentence.
  static auto describe(net::nat::holepunch::ConnRequestError) noexcept
      -> std::string_view;
  /// @brief Describes a PSN offer failure.
  ///
  /// @param error The failure.
  /// @return The PSN failure's sentence, or a fixed one.
  static auto describe(const internet::holepunch::OfferPsnError& error) noexcept
      -> std::string_view;
  /// @brief Describes a probing failure.
  ///
  /// @return A fixed sentence.
  static auto describe(internet::holepunch::OfferProbeError) noexcept
      -> std::string_view;
  /// @brief Describes an accept exchange failure.
  ///
  /// @param error The failure.
  /// @return A fixed sentence for its step.
  static auto describe(
      const internet::holepunch::SelectedAcceptExchangeError& error) noexcept
      -> std::string_view;
  /// @brief Describes a control socket handoff failure.
  ///
  /// @param error The failure.
  /// @return A fixed sentence.
  static auto describe(internet::holepunch::CtrlRudpHandoffError error) noexcept
      -> std::string_view;
  /// @brief Describes an early handoff.
  ///
  /// @return A fixed sentence.
  static auto describe(AcceptNotReady) noexcept -> std::string_view;
  /// @brief Describes a data socket adoption failure.
  ///
  /// @return A fixed sentence.
  static auto describe(Adoption) noexcept -> std::string_view;
  /// @brief Describes an accept wait failure.
  ///
  /// @param kind The failure.
  /// @return A fixed sentence.
  static auto describe(internet::holepunch::AcceptPsnErrorKind kind) noexcept
      -> std::string_view;
  /// @brief Describes an accept preparation failure.
  ///
  /// @param error The failure.
  /// @return A fixed sentence.
  static auto describe(internet::holepunch::SelectedAcceptError error) noexcept
      -> std::string_view;
  /// @brief Describes a late probe drain failure.
  ///
  /// @param error The failure.
  /// @return A fixed sentence.
  static auto describe(net::nat::holepunch::ProbeWaitError error) noexcept
      -> std::string_view;

  /// @brief The cause.
  Cause cause_;
};

}  // namespace libchiaking::play
