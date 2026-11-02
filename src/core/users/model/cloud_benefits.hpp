/// @file
/// @brief Declares CloudBenefits, what PS's cloud game feature says about a
/// user's right to stream PS5 games, and BenefitPlanId, the plans it names.
///
/// The facts come from the subscriptions service's `premium/features/cloudgame`
/// reply and are held on the user, not by the catalogue.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::users {

/// @brief The id of a benefit plan that entitles an account to cloud
/// streaming, as PS names it in `benefitPlanIds`.
///
/// Opaque to the library: the catalogue passes the ids back to PS when it
/// resolves which cloud games a plan covers. Never empty. An immutable value;
/// safe to share between threads.
class BenefitPlanId {
 public:
  /// @brief Accepts a plan id as PS wrote it.
  ///
  /// @param text The id, used verbatim.
  /// @return The id, or nothing when the text is empty.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<BenefitPlanId>;

  /// @brief The id as PS wrote it.
  ///
  /// @return The text, never empty; lives as long as this object.
  auto text() const noexcept -> const std::string& { return text_; }

  /// @brief Two ids are equal when their text is byte for byte the same.
  friend auto operator==(const BenefitPlanId&, const BenefitPlanId&)
      -> bool = default;

 private:
  /// @brief Wraps text already checked by parse().
  ///
  /// @param text The non-empty id.
  explicit BenefitPlanId(std::string text) noexcept;

  /// @brief The id, never empty.
  std::string text_;
};

/// @brief PS's statement of whether an account may stream PS5 games from
/// the cloud, with the plans and plan owners behind it.
///
/// Streaming is granted only when PS authorises it and the first owner names
/// at least one plan. Plans are always the first owner's and are never merged
/// across owners; every owner is kept as evidence. A refusal where PS did not
/// grant authorisation needs a second read, of the account's PlayStation Plus
/// services, to word what the user should do; see CloudStreaming.
///
/// Built by CatalogCalls from one reply and never changed after. A value
/// type; safe to share between threads once built.
class CloudBenefits {
 public:
  /// @brief A moment by the device's wall clock, in milliseconds since the
  /// Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief What PS's `authorization` field said.
  enum class Authorisation : std::uint8_t {
    /// @brief `authorization` was true: PS authorises cloud streaming,
    /// subject to the first owner naming a plan.
    granted,
    /// @brief `authorization` was false: PS refuses cloud streaming.
    denied,
    /// @brief The reply had no `authorization` field; treated as a
    /// refusal.
    unstated,
  };

  /// @brief One account whose subscription the benefit comes from, as PS
  /// listed it in `owners`.
  struct Owner {
    /// @brief The owning account's id as PS wrote it; it may be another
    /// account than the user's own.
    std::string account_id;
    /// @brief The owner's PlayStation Plus tier label, such as `TIER_30`,
    /// as PS wrote it.
    std::string tier_label;
    /// @brief The plans this owner's benefit covers, in PS's order;
    /// empty ids are dropped when read.
    std::vector<BenefitPlanId> plans;
    /// @brief PS's `hasEstBenefit` flag, false when absent. Kept as
    /// evidence; no rule reads it.
    bool has_est_benefit{};
    /// @brief PS's `hasSgodBenefit` flag, false when absent. Kept as
    /// evidence; no rule reads it.
    bool has_sgod_benefit{};
  };

  /// @brief Records what one cloud game feature reply said.
  ///
  /// @param authorisation PS's `authorization` field.
  /// @param justification PS's `justification` text, if any.
  /// @param owners Every owner PS listed, in its order.
  /// @param read_at When the reply was read, by the device's wall clock.
  CloudBenefits(Authorisation authorisation,
                std::optional<std::string> justification,
                std::vector<Owner> owners, Time read_at);

  /// @brief What PS said about authorisation.
  ///
  /// @return The stated authorisation.
  auto authorisation() const noexcept -> Authorisation {
    return authorisation_;
  }
  /// @brief PS's explanation of its authorisation, as PS wrote it.
  ///
  /// @return The text, or nothing when the reply carried none. Lives as long
  ///     as this object.
  auto justification() const noexcept -> const std::optional<std::string>& {
    return justification_;
  }
  /// @brief Every owner PS listed.
  ///
  /// @return The owners in PS's order, viewed in this object; empty when
  ///     PS listed none.
  auto owners() const noexcept -> std::span<const Owner> { return owners_; }
  /// @brief When the reply was read.
  ///
  /// @return The device's wall-clock time of the read.
  auto read_at() const noexcept -> Time { return read_at_; }

  /// @brief The plans that count: the first owner's.
  ///
  /// @return The first owner's plans, viewed in this object; empty when PS
  ///     listed no owner or the first owner names no plan.
  auto plans() const noexcept -> std::span<const BenefitPlanId>;
  /// @brief Whether these benefits allow cloud streaming.
  ///
  /// @return True when authorisation is granted and plans() is not empty.
  auto grants_streaming() const noexcept -> bool;
  /// @brief Whether a refusal must read the account's PlayStation Plus
  /// services to say whether the user should subscribe or upgrade.
  ///
  /// @return True when authorisation is denied or unstated. False when it is
  ///     granted, in which case a refusal (no plans) always means subscribe.
  auto refusal_needs_wording() const noexcept -> bool;

 private:
  /// @brief PS's `authorization` field.
  Authorisation authorisation_;
  /// @brief PS's `justification` text, if any.
  std::optional<std::string> justification_;
  /// @brief Every owner PS listed, in its order.
  std::vector<Owner> owners_;
  /// @brief When the reply was read, by the device's wall clock.
  Time read_at_;
};

}  // namespace libchiaking::users
