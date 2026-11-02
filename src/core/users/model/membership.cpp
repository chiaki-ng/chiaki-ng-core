#include "libchiaking/detail/users/membership.hpp"

#include <utility>

namespace libchiaking::users {

Membership::Membership(Tier tier, Plan plan)
    : known_(Known::tier), tier_(tier) {
  if (tier != Tier::none) plan_ = std::move(plan);
}

auto Membership::member_of_unknown_tier() noexcept -> Membership {
  Membership membership;
  membership.known_ = Known::member;
  return membership;
}

auto Membership::name_of(std::optional<Tier> tier) noexcept
    -> std::string_view {
  if (!tier) return "unknown";
  switch (*tier) {
    case Tier::none:
      return "none";
    case Tier::essential:
      return "essential";
    case Tier::extra:
      return "extra";
    case Tier::premium:
      return "premium";
  }
  return "unknown";
}

auto Membership::tier() const noexcept -> std::optional<Tier> {
  if (known_ != Known::tier) return std::nullopt;
  return tier_;
}

auto Membership::is_ps_plus_member() const noexcept -> std::optional<bool> {
  switch (known_) {
    case Known::member:
      return true;
    case Known::tier:
      return tier_ != Tier::none;
    case Known::nothing:
      break;
  }
  return std::nullopt;
}

}  // namespace libchiaking::users
