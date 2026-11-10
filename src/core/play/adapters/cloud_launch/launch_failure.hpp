/// @file
/// @brief Declares LaunchFailure, why a cloud launch did not reach a reserved
/// server, with the text, status and public refusal each cause implies.
#pragma once

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/play/launch_refused.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "src/core/psn/gk/error.hpp"
#include "src/core/psn/graphql/check.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::play {

/// @brief Why a cloud launch failed: an unmet precondition, a catalog
/// refusal, a refused LegacyCloudStream title step or a failed reservation.
///
/// A value; each cause can say what went wrong for the log, give the HTTP
/// status behind it and turn into the public LaunchRefused the player sees.
/// Built only through the static factories. Immutable; safe to share.
class LaunchFailure {
 public:
  /// @brief A condition the launch checks before or while it starts.
  enum class Precondition : std::uint8_t {
    /// @brief The cloud service is not supported. Not produced by the current
    /// launch.
    invalid_service,
    /// @brief The platform is not available for the service. Not produced by
    /// the
    /// current launch.
    invalid_platform,
    /// @brief The user has no PSN account to sign in with.
    invalid_npsso,
    /// @brief The bitrate is not positive.
    invalid_bitrate,
    /// @brief The title or stream settings could not be encoded.
    invalid_spec,
    /// @brief The client's device id is malformed. Not produced by the current
    /// launch.
    invalid_duid,
    /// @brief The catalog says the game is unannounced or not yet released, or
    /// trusted time is unknown.
    game_not_released,
    /// @brief The launch would use PS5 cloud streaming, which this build leaves
    /// out (`LIBCHIAKING_PS5_CLOUD_LAUNCH=OFF`).
    ps5_cloud_launch_not_built,
    /// @brief The launch would use the older PS4 cloud service, which this
    /// build leaves out (`LIBCHIAKING_PS4_CLOUD_LAUNCH=OFF`).
    ps4_cloud_launch_not_built,
  };

  /// @brief A precondition that was not met.
  class Unmet {
   public:
    /// @brief Records the unmet precondition.
    ///
    /// @param precondition What was not met.
    /// @param clearance The catalog's refusal behind a release precondition.
    explicit Unmet(
        Precondition precondition,
        std::optional<catalog::LaunchRefusal> clearance = std::nullopt);

    /// @brief Reads what was not met.
    ///
    /// @return The precondition.
    auto precondition() const noexcept -> Precondition { return precondition_; }
    /// @brief Describes it for the log.
    ///
    /// @return A fixed sentence naming the condition.
    auto describe() const noexcept -> std::string_view;
    /// @brief Gives the public refusal.
    ///
    /// @return LaunchRefused::not_cleared with the catalog's refusal when there
    ///     is one; LaunchRefused::unavailable otherwise.
    auto refused() const -> LaunchRefused;

   private:
    /// @brief What was not met.
    Precondition precondition_;
    /// @brief The catalog's refusal, for a release precondition.
    std::optional<catalog::LaunchRefusal> clearance_;
  };

  /// @brief The catalog refused to clear the title.
  class NotCleared {
   public:
    /// @brief Records the catalog's refusal.
    ///
    /// @param refusal The refusal.
    explicit NotCleared(catalog::LaunchRefusal refusal);

    /// @brief Reads the refusal.
    ///
    /// @return The catalog's refusal.
    auto refusal() const noexcept -> const catalog::LaunchRefusal& {
      return refusal_;
    }
    /// @brief Classifies a refusal the catalog could not answer, for the trace.
    ///
    /// @return Empty for a definite refusal (Premium needed, not streamable,
    ///     refused by PS, add to library, buy to stream, and the re-read
    ///     entitlement's invalid title, taken down, streaming unavailable,
    ///     purchase needed and unexpected state, and a bundle whose game must
    ///     be chosen); CheckStatus::not_found
    ///     for a title not listed or hidden; for an unavailable check the
    ///     status its cause maps to, CheckStatus::unavailable when unknown;
    ///     CheckStatus::ok for any other kind.
    auto unanswered() const noexcept
        -> std::optional<psn::graphql::CheckStatus>;
    /// @brief Describes the refusal for the log.
    ///
    /// @return A fixed sentence for the refusal's kind.
    auto describe() const noexcept -> std::string_view;
    /// @brief Gives the HTTP status behind an unanswered check.
    ///
    /// @return The status; 0 when there is none.
    auto http_status() const noexcept -> std::uint16_t;
    /// @brief Gives the public refusal.
    ///
    /// @return LaunchRefused::not_cleared with the catalog's refusal.
    auto refused() const -> LaunchRefused;

   private:
    /// @brief The catalog's refusal.
    catalog::LaunchRefusal refusal_;
  };

  /// @brief A LegacyCloudStream title step was refused.
  class TitleRefused {
   public:
    /// @brief Records the step's error.
    ///
    /// @param error The error.
    explicit TitleRefused(OldServiceLaunch::Error error);

    /// @brief Reads the error.
    ///
    /// @return The step's error.
    auto error() const noexcept -> const OldServiceLaunch::Error& {
      return error_;
    }
    /// @brief Describes the cause for the log.
    ///
    /// @return A fixed sentence for the error's cause.
    auto describe() const noexcept -> std::string_view;
    /// @brief Names the step that failed.
    ///
    /// @return A short name such as "Store title resolution".
    auto step() const noexcept -> std::string_view;
    /// @brief Gives the public refusal.
    ///
    /// @return Title not found, privacy settings (with the upgrade URL when
    ///     given), membership rejected, outside the streaming plan or licence
    ///     not free (with the price when given) for those causes;
    ///     LaunchRefused::unavailable with the HTTP status otherwise.
    auto refused() const -> LaunchRefused;

   private:
    /// @brief The step's error.
    OldServiceLaunch::Error error_;
  };

  /// @brief Reserving a server failed.
  class ReservationFailed {
   public:
    /// @brief Records the reservation error.
    ///
    /// @param error PS's or the transport's error.
    /// @param forced_datacenter The datacenter the player forced; empty when
    ///     none.
    /// @param stopped Whether the launch was cancelled.
    /// @param ps_refusal What PS's streaming service refused, when it
    ///     refused the title.
    ReservationFailed(
        psn::gk::Error error, std::string_view forced_datacenter, bool stopped,
        std::optional<catalog::StreamRefusal::Kind> ps_refusal = std::nullopt);

    /// @brief Reads the reservation error.
    ///
    /// @return The error.
    auto error() const noexcept -> const psn::gk::Error& { return error_; }
    /// @brief Reads what PS's streaming service refused.
    ///
    /// @return The refusal kind; empty when the title was not refused.
    auto ps_refusal() const noexcept
        -> std::optional<catalog::StreamRefusal::Kind> {
      return ps_refusal_;
    }
    /// @brief Describes the error for the log.
    ///
    /// @return A fixed sentence for the error's stage and detail; empty for a
    ///     stage with none.
    auto describe() const noexcept -> std::string_view;
    /// @brief Gives PS's rejection text.
    ///
    /// @return The text; empty for a cancelled or invalid request.
    auto detail() const noexcept -> std::string_view;
    /// @brief Gives the HTTP status behind the failure.
    ///
    /// @return The status; 0 when the launch was cancelled or the status was a
    ///     success or a redirect.
    auto http_status() const noexcept -> std::uint16_t;
    /// @brief Gives the public refusal.
    ///
    /// @return Refused by PS when PS refused the title; unavailable when
    ///     cancelled; membership rejected, no reachable datacenter (with the
    ///     best round trip), datacenter unavailable (for a forced datacenter)
    ///     or no server (network or queue timeout) for those reasons; sign-in
    ///     expired for an identity failure other than transport or randomness;
    ///     LaunchRefused::unavailable with the HTTP status otherwise.
    auto refused() const -> LaunchRefused;

   private:
    /// @brief PS's or the transport's error.
    psn::gk::Error error_;
    /// @brief The forced datacenter; empty when none.
    std::string forced_datacenter_;
    /// @brief Whether the launch was cancelled.
    bool stopped_;
    /// @brief What PS's streaming service refused.
    std::optional<catalog::StreamRefusal::Kind> ps_refusal_;
  };

  /// @brief One of the four causes.
  using Cause =
      std::variant<Unmet, NotCleared, TitleRefused, ReservationFailed>;

  /// @brief A precondition failed.
  ///
  /// @param failed What was not met.
  /// @return The failure.
  static auto precondition(Precondition failed) -> LaunchFailure;
  /// @brief The catalog refused the title.
  ///
  /// @param refusal The catalog's refusal.
  /// @return A release precondition for an unannounced or unreleased title or
  ///     unknown trusted time; otherwise a NotCleared cause.
  static auto not_cleared(const catalog::LaunchRefusal& refusal)
      -> LaunchFailure;
  /// @brief A LegacyCloudStream title step failed.
  ///
  /// @param error The step's error.
  /// @return The failure.
  static auto title(OldServiceLaunch::Error error) -> LaunchFailure;
  /// @brief The reservation failed.
  ///
  /// @param error The error.
  /// @param forced_datacenter The forced datacenter; empty when none.
  /// @param stopped Whether the launch was cancelled.
  /// @return The failure.
  static auto reservation(psn::gk::Error error,
                          std::string_view forced_datacenter,
                          bool stopped = false) -> LaunchFailure;
  /// @brief PS's streaming service refused the title at reservation.
  ///
  /// @param error The reservation error.
  /// @param forced_datacenter The forced datacenter; empty when none.
  /// @param kind What PS refused.
  /// @return The failure.
  static auto refused_by_service(psn::gk::Error error,
                                 std::string_view forced_datacenter,
                                 catalog::StreamRefusal::Kind kind)
      -> LaunchFailure;

  /// @brief Reads the cause.
  ///
  /// @return The cause.
  auto cause() const noexcept -> const Cause& { return cause_; }
  /// @brief Reads the cause as one alternative.
  ///
  /// @tparam T Unmet, NotCleared, TitleRefused or ReservationFailed.
  /// @return The cause when it is a @p T; null otherwise.
  template <class T>
  auto as() const noexcept -> const T* {
    return std::get_if<T>(&cause_);
  }
  /// @brief Describes the failure for the log.
  ///
  /// @return The cause's sentence.
  auto describe() const noexcept -> std::string_view;
  /// @brief Gives PS's rejection text.
  ///
  /// @return The text for a reservation failure; empty otherwise.
  auto detail() const noexcept -> std::string_view;
  /// @brief Gives the HTTP status behind the failure.
  ///
  /// @return The cause's status; 0 for a precondition or none.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief Gives the privacy settings upgrade URL.
  ///
  /// @return The URL from a LegacyCloudStream title step; empty otherwise.
  auto upgrade_url() const noexcept -> std::string_view;
  /// @brief Gives the best datacenter round trip measured.
  ///
  /// @return Milliseconds, for a reservation failure that measured one; empty
  ///     otherwise.
  auto best_rtt_ms() const noexcept -> std::optional<int>;
  /// @brief Gives the public refusal.
  ///
  /// @return The cause's refusal.
  auto refused() const -> LaunchRefused;

 private:
  /// @brief Wraps a cause.
  ///
  /// @param cause The cause.
  explicit LaunchFailure(Cause cause) noexcept : cause_(std::move(cause)) {}

  /// @brief The cause.
  Cause cause_;
};

}  // namespace libchiaking::play
