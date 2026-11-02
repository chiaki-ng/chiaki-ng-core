/// @file
/// @brief Declares usecases::QrSignIn, the use case that signs an account in
/// by a code another device approves, without a password.
#pragma once

#include "libchiaking/detail/users/qr_sign_in.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/qr_sign_in.hpp"
#include "src/core/users/usecases/sign_in.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <variant>

namespace libchiaking::users::qr_sign_in_lifecycle {
class Lifecycle;
struct Context;
}  // namespace libchiaking::users::qr_sign_in_lifecycle

namespace libchiaking::users::usecases {

/// @brief Runs a QR sign-in from the hosted registration to a held user.
///
/// Each run reads the registration afresh, obtains an application token,
/// opens a ticket and shows its code, then polls at PSN's interval until the
/// code is approved, declined or lapses. A lapsed code is replaced by a new
/// attempt, after a fresh application token when PSN asked for additional
/// verification, up to attempt_limit codes. The NPSSO an approval yields goes
/// straight into SignIn::first_time and is never handed to the caller. The
/// workflow is the qr_sign_in_lifecycle machine; this class performs its
/// effects.
///
/// Holds references to the registry's providers and sign-in use case, all of
/// which must outlive it; SignedInUsers::Core owns it. Runs may happen on
/// several threads at once; each has its own machine and waiter.
class QrSignIn {
 public:
  /// @brief Cancels the sign-in.
  using Stop = platform::StopToken;

  /// @brief The most codes one sign-in is issued before it gives up.
  static constexpr std::uint32_t attempt_limit = 10;

  /// @brief Builds the use case over a registry's providers.
  ///
  /// @param http Carries the requests. Must outlive the use case.
  /// @param clock The monotonic clock request deadlines and code lifetimes
  ///     are measured on. Must outlive the use case.
  /// @param wall Wall clock that dates when a shown code expires. Must
  ///     outlive the use case.
  /// @param sign_in The use case an approved NPSSO is signed in through. Must
  ///     outlive this one.
  /// @param waiters Makes the waiter each run pauses between polls with.
  /// @param log Receives diagnostics, or null to log nothing. Must outlive
  ///     the use case when given.
  QrSignIn(platform::HttpProvider& http, const platform::ClockProvider& clock,
           const platform::ClockProvider& wall, SignIn& sign_in,
           net::http::WaiterFactory waiters,
           platform::DiagnosticsProvider* log) noexcept;
  /// @brief Not copyable; one use case serves one registry.
  QrSignIn(const QrSignIn&) = delete;
  /// @brief Not copy-assignable; one use case serves one registry.
  auto operator=(const QrSignIn&) -> QrSignIn& = delete;

  /// @brief Signs an account in by a code another device approves.
  ///
  /// Blocks the calling thread until the sign-in ends, which can take as long
  /// as the codes last. The observer is told every state, on this thread.
  ///
  /// @param device The device id to sign in as; cloned into the new user.
  /// @param correlation_id Identifier for the authorisation request, in UUID
  ///     form.
  /// @param stop Cancels the sign-in, including a pause between polls.
  /// @param observer Told every state; may be empty.
  /// @param options The application's choices, such as the device name the
  ///     approving device shows.
  /// @return The newly held user.
  /// @retval psn::auth::SignInErrorKind::invalid_request The correlation id
  ///     or the chosen device name is not usable. Nothing was sent.
  /// @retval psn::auth::SignInErrorKind::declined The code was refused on
  ///     the approving device.
  /// @retval psn::auth::SignInErrorKind::expired Every code lapsed.
  /// @retval psn::auth::SignInErrorKind::verification_required PSN kept
  ///     asking for verification until no code was left.
  /// @retval psn::auth::SignInErrorKind::layout_changed The hosted page no
  ///     longer has the shape the registration is read from.
  /// @retval psn::auth::SignInError Any other failure of a step or of the
  ///     sign-in that follows the approval.
  [[nodiscard]] auto run(const DeviceId& device,
                         std::string_view correlation_id, const Stop& stop,
                         const QrSignInObserver& observer,
                         const QrSignInOptions& options = {})
      -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError>;

 private:
  /// @brief How a code's polling ended other than in failure.
  using Settled =
      std::variant<psn::auth::TicketApproved, psn::auth::TicketLapsed,
                   psn::auth::TicketNeedsVerification>;

  /// @brief Polls a code at PSN's interval until it settles.
  ///
  /// @param protocol The protocol client of the run.
  /// @param token The application token the ticket was opened with.
  /// @param ticket The ticket.
  /// @param stop Cancels the pauses.
  /// @param waiter Pauses between polls.
  /// @return The approval, or why the code lapsed.
  /// @retval psn::auth::SignInErrorKind::declined The code was refused.
  /// @retval psn::auth::SignInErrorKind::cancelled The stop token fired.
  /// @retval psn::auth::SignInError A poll failed.
  [[nodiscard]] auto settled(psn::auth::QrSignIn& protocol,
                             const psn::auth::ApplicationToken& token,
                             const psn::auth::Ticket& ticket, const Stop& stop,
                             net::http::Waiter& waiter)
      -> Expected<Settled, psn::auth::SignInError>;
  /// @brief Hands the queued states to the observer and drops them.
  ///
  /// @param context The machine's context.
  /// @param observer The observer; may be empty.
  static void publish(qr_sign_in_lifecycle::Context& context,
                      const QrSignInObserver& observer);
  /// @brief Ends the run in failure.
  ///
  /// @param machine The run's machine.
  /// @param context The machine's context.
  /// @param observer The observer.
  /// @param error The failure.
  /// @return The failure, to return from run().
  static auto failed(qr_sign_in_lifecycle::Lifecycle& machine,
                     qr_sign_in_lifecycle::Context& context,
                     const QrSignInObserver& observer,
                     psn::auth::SignInError error)
      -> Unexpected<psn::auth::SignInError>;
  /// @brief The monotonic time now.
  ///
  /// @return The clock's reading.
  auto steady_now() const noexcept -> std::chrono::microseconds;
  /// @brief The wall-clock time now.
  ///
  /// @return The wall clock's reading.
  auto wall_now() const noexcept -> std::chrono::system_clock::time_point;

  /// @brief Carries the requests.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Wall clock that dates code expiry.
  const platform::ClockProvider& wall_;
  /// @brief The use case an approved NPSSO is signed in through.
  SignIn& sign_in_;
  /// @brief Makes each run's waiter.
  net::http::WaiterFactory waiters_;
  /// @brief Receives diagnostics; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::users::usecases
