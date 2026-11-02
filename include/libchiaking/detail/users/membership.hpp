/// @file
/// @brief Declares Membership, what is known about a user's PlayStation Plus
/// subscription.
#pragma once

#include "libchiaking/detail/users/account_id.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::users {

/// @brief A PlayStation Plus tier, in ascending order.
enum class Tier : std::uint8_t {
  /// @brief Not a member.
  none,
  /// @brief PlayStation Plus Essential.
  essential,
  /// @brief PlayStation Plus Extra.
  extra,
  /// @brief PlayStation Plus Premium.
  premium,
};

/// @brief What is known about a user's PlayStation Plus membership.
///
/// Knowledge comes in three strengths. Nothing may be known. The user may be
/// known to be a member without the tier being known, which is all a profile
/// says. Or the tier may be known, including that the user is not a member at
/// all. tier() and is_ps_plus_member() return empty values rather than guesses
/// when the knowledge is missing.
class Membership {
 public:
  /// @brief The subscription as PSN describes it. Every field is PSN's own
  /// text and may be empty.
  struct Plan {
    /// @brief PSN's label for the tier.
    std::string tier_label;
    /// @brief The identifier of the subscription product.
    std::string sku_id;
    /// @brief The display name of the subscription product.
    std::string product_name;
    /// @brief When the subscription ends, in the form PSN states it.
    std::string end_date;
  };

  /// @brief Creates a membership about which nothing is known.
  Membership() noexcept = default;
  /// @brief Creates a membership whose tier is known.
  ///
  /// @param tier The tier. Tier::none records that the user is known not to
  ///     be a member.
  /// @param plan The subscription details. Discarded when the tier is
  ///     Tier::none.
  explicit Membership(Tier tier, Plan plan = {});

  /// @brief Creates a membership for a user known to be a member whose tier
  /// is not known.
  ///
  /// @return A membership that records a member with no known tier.
  static auto member_of_unknown_tier() noexcept -> Membership;
  /// @brief Names a tier for diagnostics.
  ///
  /// @param tier The tier, or nothing for an unknown one.
  /// @return "none", "essential", "extra" or "premium", or "unknown" when no
  ///     tier is given.
  static auto name_of(std::optional<Tier> tier) noexcept -> std::string_view;

  /// @brief The tier, when it is known.
  ///
  /// @return The tier, which may be Tier::none, or nothing when the tier is
  ///     not known, even if the user is known to be a member.
  auto tier() const noexcept -> std::optional<Tier>;
  /// @brief Whether the user is a PlayStation Plus member.
  ///
  /// @return True or false when known, or nothing when nothing is known.
  auto is_ps_plus_member() const noexcept -> std::optional<bool>;
  /// @brief The subscription details. Empty unless a tier above Tier::none is
  /// known.
  ///
  /// @return The details, which live as long as this membership; empty when no
  /// tier above Tier::none is known.
  auto plan() const noexcept -> const Plan& { return plan_; }

 private:
  enum class Known : std::uint8_t { nothing, member, tier };

  Known known_{Known::nothing};
  Tier tier_{Tier::none};
  Plan plan_;
};

/// @brief A membership together with where and when it was learned.
///
/// @see PsUser::get_my_membership
struct MembershipAnswer {
  /// @brief The account the membership belongs to.
  AccountId account;
  /// @brief What is known about the membership.
  Membership membership;
  /// @brief Whether the answer was served from the library's cache instead of
  /// being fetched for this call.
  bool cached{};
  /// @brief When the answer was fetched, in milliseconds since the Unix
  /// epoch.
  std::int64_t fetched_at_ms{};
};

}  // namespace libchiaking::users
