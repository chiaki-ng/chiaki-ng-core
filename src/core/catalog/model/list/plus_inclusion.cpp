#include "libchiaking/detail/catalog/plus_inclusion.hpp"

namespace libchiaking::catalog {

PlusInclusion::PlusInclusion(std::optional<users::Tier> lowest_tier,
                             Source source) noexcept
    : lowest_tier_(lowest_tier), source_(source) {}

auto PlusInclusion::not_included(Source source) noexcept -> PlusInclusion {
  return PlusInclusion{std::nullopt, source};
}

auto PlusInclusion::from_tier(users::Tier lowest_tier, Source source) noexcept
    -> std::optional<PlusInclusion> {
  if (lowest_tier == users::Tier::none) return std::nullopt;
  return PlusInclusion{lowest_tier, source};
}

auto PlusInclusion::with_premium(Source source) noexcept -> PlusInclusion {
  return PlusInclusion{users::Tier::premium, source};
}

auto PlusInclusion::covers(const users::Membership& membership) const noexcept
    -> std::optional<bool> {
  if (!lowest_tier_) return false;
  const auto held = membership.tier();
  if (!held) return std::nullopt;
  return *held >= *lowest_tier_;
}

auto PlusInclusion::combined_with(const PlusInclusion& other) const noexcept
    -> PlusInclusion {
  return other.outranks(*this) ? other : *this;
}

auto PlusInclusion::outranks(const PlusInclusion& other) const noexcept
    -> bool {
  if (source_ != other.source_) return source_ < other.source_;
  if (lowest_tier_.has_value() != other.lowest_tier_.has_value())
    return lowest_tier_.has_value();
  return lowest_tier_ && *lowest_tier_ < *other.lowest_tier_;
}

}  // namespace libchiaking::catalog
