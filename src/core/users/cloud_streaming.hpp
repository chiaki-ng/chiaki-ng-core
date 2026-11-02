/// @file
/// @brief Declares CloudStreaming, the verdict on whether a user may stream
/// PS5 games from the cloud, and PlusService, the PlayStation Plus fact that
/// words a refusal.
///
/// Benefits that cannot be read leave the verdict unknown, which blocks a
/// launch; there is no fallback to the profile's PlayStation Plus flag.
#pragma once

#include "src/core/users/model/cloud_benefits.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::users {

/// @brief The account's first PlayStation Plus service, as the subscriptions
/// service's `pssubscriptions` reply lists it.
///
/// Read only when cloud streaming is refused without authorisation, and used
/// only to word the refusal: a subscriber on a lower tier is invited to
/// upgrade rather than subscribe. An immutable value; safe to share between
/// threads.
class PlusService {
 public:
  /// @brief Records the service as PS described it.
  ///
  /// @param active Whether PS marked the service active.
  /// @param status PS's subscription status, such as `SUBSCRIBED`.
  /// @param tier_label PS's tier label, such as `TIER_10`.
  PlusService(bool active, std::string status, std::string tier_label) noexcept;

  /// @brief Whether the user already subscribes on a tier below the one that
  /// includes cloud streaming.
  ///
  /// @return True when the service is active, `SUBSCRIBED` and on the
  ///     Essential (`TIER_10`) or Extra (`TIER_20`) tier.
  auto invites_an_upgrade() const noexcept -> bool;

 private:
  /// @brief PS's status for a current subscription.
  static constexpr std::string_view subscribed = "SUBSCRIBED";
  /// @brief PS's tier label for PlayStation Plus Essential.
  static constexpr std::string_view essential_tier = "TIER_10";
  /// @brief PS's tier label for PlayStation Plus Extra.
  static constexpr std::string_view extra_tier = "TIER_20";

  /// @brief Whether PS marked the service active.
  bool active_;
  /// @brief PS's subscription status, verbatim.
  std::string status_;
  /// @brief PS's tier label, verbatim.
  std::string tier_label_;
};

/// @brief Whether a user may stream PS5 games from the cloud: allowed with the
/// plans that cover it, refused with what the user should do, or unknown
/// because PS could not be asked.
///
/// Exactly one of three shapes holds:
/// - allowed: benefits() is set, remedy() and cause() are empty;
/// - refused: benefits() and remedy() are set, cause() is empty;
/// - unknown: only cause() is set.
///
/// Made by CatalogCalls::cloud_streaming and held per user by
/// CloudStreamingCache. A value type; safe to share between threads.
class CloudStreaming {
 public:
  /// @brief What a refused user should do to be allowed to stream.
  enum class Remedy : std::uint8_t {
    /// @brief Subscribe to the PlayStation Plus tier that includes cloud
    /// streaming.
    subscribe,
    /// @brief Upgrade an existing Essential or Extra subscription.
    upgrade,
  };

  /// @brief Judges what PS said about the account's benefits.
  ///
  /// @param benefits What the cloud game feature reply said; consumed.
  /// @param first_plus The account's first PlayStation Plus service, read
  ///     only when benefits.refusal_needs_wording(); nothing when it was not
  ///     read, could not be read or the account has none.
  /// @return Allowed when the benefits grant streaming. Otherwise refused,
  ///     with Remedy::upgrade when the benefits need wording and
  ///     `first_plus` invites an upgrade, and Remedy::subscribe in every
  ///     other case.
  [[nodiscard]] static auto of(CloudBenefits benefits,
                               const std::optional<PlusService>& first_plus =
                                   std::nullopt) -> CloudStreaming;
  /// @brief Makes the verdict for benefits that could not be read.
  ///
  /// @param cause Why PS gave no answer.
  /// @return An unknown verdict carrying the cause.
  [[nodiscard]] static auto unknown(Unanswered cause) noexcept
      -> CloudStreaming;

  /// @brief Whether the user may stream.
  ///
  /// @return True only for an allowed verdict; false when refused or unknown.
  auto is_allowed() const noexcept -> bool;
  /// @brief Whether the catalogue must hide cloud tiles because the user's
  /// tier does not include streaming.
  ///
  /// @return True only for a refused verdict; an unknown verdict hides
  ///     nothing.
  auto hides_tiles() const noexcept -> bool;
  /// @brief The plans that cover the user's streaming.
  ///
  /// @return The first owner's plans for an allowed verdict, never empty
  ///     then; empty when refused or unknown. Viewed in this object.
  auto plans() const noexcept -> std::span<const BenefitPlanId>;
  /// @brief What a refused user should do.
  ///
  /// @return The remedy for a refused verdict; nothing when allowed or
  ///     unknown.
  auto remedy() const noexcept -> std::optional<Remedy> { return remedy_; }
  /// @brief Why the verdict is unknown.
  ///
  /// @return The failure for an unknown verdict; nothing when allowed or
  ///     refused.
  auto cause() const noexcept -> std::optional<Unanswered> { return cause_; }
  /// @brief The benefits the verdict was judged from.
  ///
  /// @return The benefits when allowed or refused; nothing when unknown.
  ///     Lives as long as this object.
  auto benefits() const noexcept -> const std::optional<CloudBenefits>& {
    return benefits_;
  }

 private:
  /// @brief Builds one of the three shapes; of() and unknown() choose which.
  ///
  /// @param benefits The benefits, or nothing when unknown.
  /// @param remedy The remedy, or nothing when allowed or unknown.
  /// @param cause The failure, or nothing when allowed or refused.
  CloudStreaming(std::optional<CloudBenefits> benefits,
                 std::optional<Remedy> remedy,
                 std::optional<Unanswered> cause) noexcept;

  /// @brief Words a refusal that PS did not authorise.
  ///
  /// @param first_plus The account's first PlayStation Plus service, if read.
  /// @return Remedy::upgrade when the service invites an upgrade, otherwise
  ///     Remedy::subscribe.
  static auto remedy_for(const std::optional<PlusService>& first_plus) noexcept
      -> Remedy;

  /// @brief What PS said; empty for an unknown verdict.
  std::optional<CloudBenefits> benefits_;
  /// @brief What a refused user should do; empty unless refused.
  std::optional<Remedy> remedy_;
  /// @brief Why PS gave no answer; empty unless unknown.
  std::optional<Unanswered> cause_;
};

}  // namespace libchiaking::users
