/// @file
/// @brief Declares WakeFailure, why waking a console through PSN, or waiting
/// for it to be ready, failed.
#pragma once

#include "src/core/play/adapters/links/internet/psn_failure.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace libchiaking::play {

/// @brief A failure to wake the console and see it ready: a refusal before
/// anything was sent, or a PSN failure. A value.
class WakeFailure {
 public:
  /// @enum Refusal
  /// @brief Why the wake did not begin.
  ///
  /// @var Refusal::invalid_state
  /// @brief The PSN session was not open or was already being released.
  ///
  /// @var Refusal::timed_out
  /// @brief The start's deadline had already passed.
  ///
  /// @var Refusal::cancelled
  /// @brief The start was stopped.
  enum class Refusal : std::uint8_t { invalid_state, timed_out, cancelled };

  /// @brief A refusal or a PSN failure.
  using Cause = std::variant<Refusal, PsnFailure>;

  /// @brief Builds a refusal.
  ///
  /// @param refusal Why.
  WakeFailure(Refusal refusal) noexcept : cause_(refusal) {}
  /// @brief Builds a PSN failure.
  ///
  /// @param failure The failure.
  WakeFailure(PsnFailure failure) noexcept : cause_(std::move(failure)) {}

  /// @brief Reads the cause as one alternative.
  ///
  /// @tparam T Refusal or PsnFailure.
  /// @return The cause when it is a @p T; null otherwise.
  template <class T>
  auto as() const noexcept -> const T* {
    return std::get_if<T>(&cause_);
  }
  /// @brief Describes the failure.
  ///
  /// @return The PSN failure's sentence, or a fixed sentence for the refusal.
  auto describe() const noexcept -> std::string_view {
    if (const auto* psn = as<PsnFailure>()) return psn->describe();
    switch (std::get<Refusal>(cause_)) {
      case Refusal::invalid_state:
        return "the PSN readiness state was invalid";
      case Refusal::timed_out:
        return "the PSN readiness deadline expired";
      case Refusal::cancelled:
        break;
    }
    return "the PSN readiness operation was cancelled";
  }
  /// @brief Reports whether the failure was a cancellation.
  ///
  /// @return `true` for a cancelled refusal or PSN failure.
  auto cancelled() const noexcept -> bool {
    if (const auto* psn = as<PsnFailure>()) return psn->cancelled();
    return std::get<Refusal>(cause_) == Refusal::cancelled;
  }
  /// @brief Gives the HTTP status behind the failure.
  ///
  /// @return The PSN failure's status; 0 for a refusal.
  auto http_status() const noexcept -> std::uint16_t {
    const auto* psn = as<PsnFailure>();
    return psn ? psn->http_status() : std::uint16_t{};
  }

 private:
  /// @brief The cause.
  Cause cause_;
};

}  // namespace libchiaking::play
