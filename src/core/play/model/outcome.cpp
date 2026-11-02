#include "libchiaking/detail/play/outcome.hpp"

#include <utility>

namespace libchiaking::play {

ConsoleQuit::ConsoleQuit(Reason reason, std::string_view text)
    : reason_(reason), text_(text.substr(0, text_limit)) {}

EndReason::EndReason(Cause cause) : cause_(cause) {}

EndReason::EndReason(Cause cause, Facts facts)
    : cause_(cause), facts_(std::move(facts)) {
  if (facts_.action_url.size() > url_limit) facts_.action_url.resize(url_limit);
  if (facts_.detail.size() > detail_limit) facts_.detail.resize(detail_limit);
}

Outcome::Outcome(Evidence evidence) : evidence_(std::move(evidence)) {
  classify();
}

auto Outcome::not_started(EndReason::Cause cause) -> Outcome {
  Evidence evidence;
  evidence.not_started = cause;
  return Outcome{std::move(evidence)};
}

auto Outcome::reason() const noexcept -> const EndReason* {
  return evidence_.primary && !evidence_.primary->was_left()
             ? &*evidence_.primary
             : nullptr;
}

auto Outcome::cleanup() const noexcept -> const EndReason* {
  return evidence_.cleanup ? &*evidence_.cleanup : nullptr;
}

auto Outcome::launch_refused() const noexcept -> const LaunchRefused* {
  const auto* failure = reason();
  return failure != nullptr && failure->launch_refused()
             ? &*failure->launch_refused()
             : nullptr;
}

void Outcome::settle(Ending ending,
                     std::optional<EndReason::Cause> cause) noexcept {
  ending_ = ending;
  cause_ = cause;
}

void Outcome::classify() noexcept {
  using Cause = EndReason::Cause;
  if (const auto refused = evidence_.not_started) {
    settle(Ending::not_started, refused);
    return;
  }
  const bool primary_failed =
      evidence_.media.has_value() || reason() != nullptr;
  const bool cleanup_failed =
      evidence_.stop_cause.has_value() || evidence_.cleanup.has_value();
  if (evidence_.left && !primary_failed && !cleanup_failed) {
    settle(Ending::left, std::nullopt);
  } else if (!primary_failed && cleanup_failed) {
    settle(Ending::shutdown_failed, evidence_.stop_cause
                                        ? *evidence_.stop_cause
                                        : evidence_.cleanup->cause());
  } else if (evidence_.phase == Phase::media && evidence_.media) {
    settle(Ending::media_failed, std::nullopt);
  } else if (evidence_.phase == Phase::start && evidence_.phase_cause) {
    settle(Ending::start_failed, evidence_.phase_cause);
  } else if (evidence_.phase == Phase::session && evidence_.phase_cause) {
    settle(Ending::session_failed, evidence_.phase_cause);
  } else if (evidence_.phase != Phase::none) {
    settle(Ending::session_failed, Cause::pipeline_failed);
  } else if (const auto stop = evidence_.stop_cause) {
    settle(Ending::shutdown_failed, stop);
  } else {
    settle(Ending::finished, std::nullopt);
  }
  const auto& quit = evidence_.quit;
  if (ending_ == Ending::finished && quit &&
      quit->reason() != ConsoleQuit::Reason::stopped)
    settle(Ending::console_quit, std::nullopt);
}

}  // namespace libchiaking::play
