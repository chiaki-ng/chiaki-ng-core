#include "src/core/users/usecases/qr_sign_in.hpp"

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/users/model/qr_sign_in_lifecycle.hpp"
#include "src/core/utils/text/uuid.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace libchiaking::users::usecases {

QrSignIn::QrSignIn(platform::HttpProvider& http,
                   const platform::ClockProvider& clock,
                   const platform::ClockProvider& wall, SignIn& sign_in,
                   net::http::WaiterFactory waiters,
                   platform::DiagnosticsProvider* log) noexcept
    : http_(http),
      clock_(clock),
      wall_(wall),
      sign_in_(sign_in),
      waiters_(std::move(waiters)),
      log_(log) {}

auto QrSignIn::run(const DeviceId& device, std::string_view correlation_id,
                   const Stop& stop, const QrSignInObserver& observer,
                   const QrSignInOptions& options)
    -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError> {
  using namespace qr_sign_in_lifecycle;
  using psn::auth::SignInError;
  using psn::auth::SignInErrorKind;
  Context context{.attempt_limit = attempt_limit};
  Lifecycle machine{context};
  if (!machine.process(Begin{}))
    return unexpected(SignInError{SignInErrorKind::invalid_request});
  publish(context, observer);
  if (!text::Uuid::parse(correlation_id))
    return failed(machine, context, observer,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the correlation id is not a UUID"});
  if (!options.device_name_usable())
    return failed(machine, context, observer,
                  SignInError{SignInErrorKind::invalid_request, 0,
                              "the device name is not usable"});
  const auto waiter = waiters_();
  psn::auth::QrSignIn protocol{http_, clock_, stop, log_};
  const auto registration = protocol.registration();
  if (!registration)
    return failed(machine, context, observer, registration.error());
  auto token = protocol.application_token(registration.value());
  if (!token) return failed(machine, context, observer, token.error());
  for (;;) {
    const auto attempt = context.attempt;
    const auto ticket = protocol.ticket(token.value(), options.device_name());
    if (!ticket) return failed(machine, context, observer, ticket.error());
    const auto& issued = ticket.value();
    if (!machine.process(Issued{attempt,
                                psn::auth::QrSignIn::approval_url(issued),
                                SecretString{issued.user_code.view()},
                                wall_now() + issued.expires_in}))
      return failed(machine, context, observer,
                    SignInError{SignInErrorKind::unavailable});
    publish(context, observer);
    auto outcome = settled(protocol, token.value(), issued, stop, *waiter);
    if (!outcome) return failed(machine, context, observer, outcome.error());
    if (auto* approved =
            std::get_if<psn::auth::TicketApproved>(&outcome.value())) {
      if (!machine.process(Approved{attempt}))
        return failed(machine, context, observer,
                      SignInError{SignInErrorKind::unavailable});
      publish(context, observer);
      auto kept =
          sign_in_.first_time({approved->npsso}, device, correlation_id, stop);
      if (!kept) return failed(machine, context, observer, kept.error());
      if (machine.process(Kept{attempt})) publish(context, observer);
      return std::move(kept.value());
    }
    const bool verification =
        std::holds_alternative<psn::auth::TicketNeedsVerification>(
            outcome.value());
    if (!machine.process(Replace{
            attempt, verification ? Lapse::verification : Lapse::expired}))
      return failed(machine, context, observer,
                    SignInError{SignInErrorKind::unavailable});
    publish(context, observer);
    if (machine.step() == QrSignInStep::failed)
      return unexpected(
          SignInError{context.failure.value_or(SignInErrorKind::expired)}.at(
              "users.qr_sign_in"));
    if (verification) {
      token = protocol.application_token(registration.value());
      if (!token) return failed(machine, context, observer, token.error());
    }
  }
}

auto QrSignIn::settled(psn::auth::QrSignIn& protocol,
                       const psn::auth::ApplicationToken& token,
                       const psn::auth::Ticket& ticket, const Stop& stop,
                       net::http::Waiter& waiter)
    -> Expected<Settled, psn::auth::SignInError> {
  using psn::auth::SignInError;
  using psn::auth::SignInErrorKind;
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  const auto deadline = steady_now() + ticket.expires_in;
  auto pause = duration_cast<milliseconds>(ticket.interval);
  for (;;) {
    const auto left = duration_cast<milliseconds>(deadline - steady_now());
    const auto wait = std::clamp(std::min(pause, left), milliseconds{0},
                                 milliseconds{std::numeric_limits<int>::max()});
    if (stop.stop_requested() ||
        !waiter.wait_milliseconds(static_cast<int>(wait.count()), stop) ||
        stop.stop_requested())
      return unexpected(SignInError{SignInErrorKind::cancelled, 0,
                                    "the sign-in was cancelled"}
                            .at("users.qr_sign_in"));
    if (steady_now() >= deadline) return Settled{psn::auth::TicketLapsed{}};
    auto answer = protocol.poll(token, ticket.handle);
    if (!answer) return unexpected(answer.error());
    pause = duration_cast<milliseconds>(ticket.interval);
    auto next = std::visit(
        [&pause, &ticket](
            auto& reply) -> std::optional<Expected<Settled, SignInError>> {
          using Reply = std::remove_cvref_t<decltype(reply)>;
          if constexpr (std::is_same_v<Reply, psn::auth::TicketPending>) {
            return std::nullopt;
          } else if constexpr (std::is_same_v<Reply,
                                              psn::auth::TicketSlowDown>) {
            pause = duration_cast<milliseconds>(
                std::max(ticket.interval, reply.retry_after));
            return std::nullopt;
          } else if constexpr (std::is_same_v<Reply,
                                              psn::auth::TicketDeclined>) {
            return Expected<Settled, SignInError>{
                unexpected(SignInError{SignInErrorKind::declined}.at(
                    "psn.auth.qr_sign_in.poll"))};
          } else {
            return Expected<Settled, SignInError>{Settled{std::move(reply)}};
          }
        },
        answer.value());
    if (next) return std::move(*next);
  }
}

void QrSignIn::publish(qr_sign_in_lifecycle::Context& context,
                       const QrSignInObserver& observer) {
  while (!context.effects.empty()) {
    const auto state = std::move(context.effects.front());
    context.effects.pop_front();
    if (observer) observer(state);
  }
}

auto QrSignIn::failed(qr_sign_in_lifecycle::Lifecycle& machine,
                      qr_sign_in_lifecycle::Context& context,
                      const QrSignInObserver& observer,
                      psn::auth::SignInError error)
    -> Unexpected<psn::auth::SignInError> {
  if (machine.process(
          qr_sign_in_lifecycle::Failed{context.attempt, error.kind()}))
    publish(context, observer);
  return unexpected(error);
}

auto QrSignIn::steady_now() const noexcept -> std::chrono::microseconds {
  return std::chrono::microseconds{clock_.now_microseconds()};
}

auto QrSignIn::wall_now() const noexcept
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point{
      std::chrono::microseconds{wall_.now_microseconds()}};
}

}  // namespace libchiaking::users::usecases
