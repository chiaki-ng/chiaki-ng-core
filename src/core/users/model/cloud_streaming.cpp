#include "src/core/users/cloud_streaming.hpp"

#include <utility>

namespace libchiaking::users {

PlusService::PlusService(bool active, std::string status,
                         std::string tier_label) noexcept
    : active_(active),
      status_(std::move(status)),
      tier_label_(std::move(tier_label)) {}

auto PlusService::invites_an_upgrade() const noexcept -> bool {
  return active_ && status_ == subscribed &&
         (tier_label_ == essential_tier || tier_label_ == extra_tier);
}

CloudStreaming::CloudStreaming(std::optional<CloudBenefits> benefits,
                               std::optional<Remedy> remedy,
                               std::optional<Unanswered> cause) noexcept
    : benefits_(std::move(benefits)), remedy_(remedy), cause_(cause) {}

auto CloudStreaming::of(CloudBenefits benefits,
                        const std::optional<PlusService>& first_plus)
    -> CloudStreaming {
  if (benefits.grants_streaming())
    return CloudStreaming{std::move(benefits), std::nullopt, std::nullopt};
  const auto remedy = benefits.refusal_needs_wording() ? remedy_for(first_plus)
                                                       : Remedy::subscribe;
  return CloudStreaming{std::move(benefits), remedy, std::nullopt};
}

auto CloudStreaming::unknown(Unanswered cause) noexcept -> CloudStreaming {
  return CloudStreaming{std::nullopt, std::nullopt, cause};
}

auto CloudStreaming::is_allowed() const noexcept -> bool {
  return benefits_ && !remedy_;
}

auto CloudStreaming::hides_tiles() const noexcept -> bool {
  return remedy_.has_value();
}

auto CloudStreaming::plans() const noexcept -> std::span<const BenefitPlanId> {
  if (!is_allowed()) return {};
  return benefits_->plans();
}

auto CloudStreaming::remedy_for(
    const std::optional<PlusService>& first_plus) noexcept -> Remedy {
  return first_plus && first_plus->invites_an_upgrade() ? Remedy::upgrade
                                                        : Remedy::subscribe;
}

}  // namespace libchiaking::users
