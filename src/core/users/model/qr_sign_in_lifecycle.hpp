/// @file
/// @brief Declares the state machine of a QR sign-in: preparing, awaiting
/// approval of a code, signing in, and the two ends, signed in and failed.
///
/// The machine only decides; it performs nothing. Each transition updates the
/// Context and queues the new QrSignInState for the observer. Every event but
/// Begin carries the attempt it belongs to; one from an older attempt is
/// counted and dropped, as is one the table does not allow from the current
/// state, so a stale answer about a replaced code can never revive it.
#pragma once

#include "libchiaking/detail/users/qr_sign_in.hpp"

#include <boost/sml.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::users::qr_sign_in_lifecycle {

/// @brief Boost.SML, which the machine is written in.
namespace sml = boost::sml;

/// @brief Nothing has started.
struct IdleState {};
/// @brief The registration, the token or a code is being obtained.
struct PreparingState {};
/// @brief A code is shown and polled.
struct AwaitingApprovalState {};
/// @brief The code was approved and the account is being signed in.
struct SigningInState {};
/// @brief The account is signed in.
struct SignedInState {};
/// @brief The sign-in ended without an account.
struct FailedState {};

/// @brief Why a code is replaced.
enum class Lapse : std::uint8_t {
  /// @brief The code expired or PSN no longer accepts it.
  expired,
  /// @brief PSN asked for additional verification.
  verification,
};

/// @brief The sign-in starts, with its first attempt.
struct Begin {};
/// @brief PSN issued the attempt's code.
struct Issued {
  /// @brief The attempt the event belongs to.
  std::uint32_t attempt_id{};
  /// @brief The address shown as the QR code.
  SecretString approval_url;
  /// @brief The six digits shown beside it.
  SecretString user_code;
  /// @brief When the code stops being accepted, by the wall clock.
  std::chrono::system_clock::time_point expires_at{};
};
/// @brief The attempt's code lapsed; a replacement is wanted.
struct Replace {
  /// @brief The attempt the event belongs to.
  std::uint32_t attempt_id{};
  /// @brief Why the code lapsed.
  Lapse lapse{Lapse::expired};
};
/// @brief The attempt's code was approved.
struct Approved {
  /// @brief The attempt the event belongs to.
  std::uint32_t attempt_id{};
};
/// @brief The approved account is signed in and held.
struct Kept {
  /// @brief The attempt the event belongs to.
  std::uint32_t attempt_id{};
};
/// @brief The sign-in fails.
struct Failed {
  /// @brief The attempt the event belongs to.
  std::uint32_t attempt_id{};
  /// @brief Why.
  SignInErrorKind kind{SignInErrorKind::transport};
};

/// @brief What the machine knows beside its state, and the states it queued.
///
/// Owned by the caller and borrowed by Lifecycle; drain the effects after
/// each event.
struct Context {
  /// @brief The most codes one sign-in may be issued.
  std::uint32_t attempt_limit{};
  /// @brief States to hand to the observer, oldest first.
  std::deque<QrSignInState> effects{};
  /// @brief The attempt in progress; 0 before Begin.
  std::uint32_t attempt{};
  /// @brief Whether the sign-in is still running.
  bool running{};
  /// @brief The step reached, as published.
  QrSignInStep step{QrSignInStep::preparing};
  /// @brief The address shown for the current code.
  SecretString approval_url{};
  /// @brief The digits shown for the current code.
  SecretString user_code{};
  /// @brief When the current code stops being accepted.
  std::optional<std::chrono::system_clock::time_point> expires_at{};
  /// @brief Why the sign-in failed.
  std::optional<SignInErrorKind> failure{};
  /// @brief Events dropped because they named another attempt.
  std::size_t stale_events{};
  /// @brief Events dropped because the table does not allow them now.
  std::size_t unexpected_events{};

  /// @brief Whether an event belongs to the attempt in progress.
  ///
  /// @param attempt_id The event's attempt.
  /// @return True when the sign-in runs and the event names its attempt.
  auto is_current(std::uint32_t attempt_id) const noexcept -> bool {
    return running && attempt_id == attempt;
  }
  /// @brief Whether another code may still be issued.
  ///
  /// @return True while fewer than attempt_limit codes were issued.
  auto has_room() const noexcept -> bool { return attempt < attempt_limit; }
  /// @brief Forgets the code shown.
  void hide_code() noexcept {
    approval_url.clear();
    user_code.clear();
    expires_at.reset();
  }
  /// @brief Moves to a step and queues the state as it stands.
  ///
  /// @param next The step reached.
  void publish(QrSignInStep next) {
    step = next;
    effects.emplace_back(attempt, step, approval_url.clone(), user_code.clone(),
                         expires_at, failure);
  }
};

/// @brief The transition table.
struct LifecycleFsm {
  /// @brief Builds the table.
  ///
  /// @return The SML transition table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto begin = [](Context& context) {
      context.attempt = 1;
      context.running = true;
      context.hide_code();
      context.failure.reset();
      context.publish(QrSignInStep::preparing);
    };
    const auto show = [](const Issued& event, Context& context) {
      context.approval_url = event.approval_url.clone();
      context.user_code = event.user_code.clone();
      context.expires_at = event.expires_at;
      context.publish(QrSignInStep::awaiting_approval);
    };
    const auto renew = [](Context& context) {
      ++context.attempt;
      context.hide_code();
      context.publish(QrSignInStep::preparing);
    };
    const auto exhaust = [](const Replace& event, Context& context) {
      context.hide_code();
      context.running = false;
      context.failure = event.lapse == Lapse::verification
                            ? SignInErrorKind::verification_required
                            : SignInErrorKind::expired;
      context.publish(QrSignInStep::failed);
    };
    const auto approve = [](Context& context) {
      context.hide_code();
      context.publish(QrSignInStep::signing_in);
    };
    const auto keep = [](Context& context) {
      context.running = false;
      context.publish(QrSignInStep::signed_in);
    };
    const auto fail = [](const Failed& event, Context& context) {
      context.hide_code();
      context.running = false;
      context.failure = event.kind;
      context.publish(QrSignInStep::failed);
    };
    const auto room = [](const Context& context) { return context.has_room(); };
    const auto full = [](const Context& context) {
      return !context.has_room();
    };
    return make_transition_table(
        *state<IdleState> + event<Begin> / begin = state<PreparingState>,
        state<PreparingState> + event<Issued> / show =
            state<AwaitingApprovalState>,
        state<AwaitingApprovalState> + event<Replace>[room] / renew =
            state<PreparingState>,
        state<AwaitingApprovalState> + event<Replace>[full] / exhaust =
            state<FailedState>,
        state<AwaitingApprovalState> + event<Approved> / approve =
            state<SigningInState>,
        state<SigningInState> + event<Kept> / keep = state<SignedInState>,
        state<PreparingState> + event<Failed> / fail = state<FailedState>,
        state<AwaitingApprovalState> + event<Failed> / fail =
            state<FailedState>,
        state<SigningInState> + event<Failed> / fail = state<FailedState>);
  }
};

/// @brief One sign-in's machine over a borrowed context.
///
/// Not thread-safe; one sign-in drives it from its own thread.
class Lifecycle {
 public:
  /// @brief Starts the machine idle over a context.
  ///
  /// @param context The context the machine reads and writes; must outlive
  ///     the machine.
  explicit Lifecycle(Context& context) : context_(context), machine_(context) {}

  /// @brief Starts the sign-in.
  ///
  /// @param event The start.
  /// @return Whether the machine accepted it; only an idle machine does.
  [[nodiscard]] auto process(const Begin& event) -> bool {
    return accepted(event);
  }
  /// @brief Feeds an event of an attempt.
  ///
  /// @tparam Event One of the attempt-tagged events.
  /// @param event The event.
  /// @return Whether the machine accepted it; an event of another attempt is
  ///     counted as stale, one the table does not allow as unexpected.
  template <class Event>
  [[nodiscard]] auto process(const Event& event) -> bool {
    if (!context_.is_current(event.attempt_id)) {
      ++context_.stale_events;
      return false;
    }
    return accepted(event);
  }
  /// @brief The step the machine is in.
  ///
  /// @return The step; QrSignInStep::preparing while idle.
  auto step() const noexcept -> QrSignInStep {
    if (machine_.is(sml::state<AwaitingApprovalState>))
      return QrSignInStep::awaiting_approval;
    if (machine_.is(sml::state<SigningInState>))
      return QrSignInStep::signing_in;
    if (machine_.is(sml::state<SignedInState>)) return QrSignInStep::signed_in;
    if (machine_.is(sml::state<FailedState>)) return QrSignInStep::failed;
    return QrSignInStep::preparing;
  }

 private:
  /// @brief Feeds an event to the machine, counting a refusal.
  ///
  /// @tparam Event The event type.
  /// @param event The event.
  /// @return Whether the table allowed it.
  template <class Event>
  [[nodiscard]] auto accepted(const Event& event) -> bool {
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief The borrowed context.
  Context& context_;
  /// @brief The SML machine.
  sml::sm<LifecycleFsm> machine_;
};

}  // namespace libchiaking::users::qr_sign_in_lifecycle
