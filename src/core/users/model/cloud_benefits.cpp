#include "src/core/users/model/cloud_benefits.hpp"

#include <utility>

namespace libchiaking::users {

auto BenefitPlanId::parse(std::string_view text)
    -> std::optional<BenefitPlanId> {
  if (text.empty()) return std::nullopt;
  return BenefitPlanId{std::string{text}};
}

BenefitPlanId::BenefitPlanId(std::string text) noexcept
    : text_(std::move(text)) {}

CloudBenefits::CloudBenefits(Authorisation authorisation,
                             std::optional<std::string> justification,
                             std::vector<Owner> owners, Time read_at)
    : authorisation_(authorisation),
      justification_(std::move(justification)),
      owners_(std::move(owners)),
      read_at_(read_at) {}

auto CloudBenefits::plans() const noexcept -> std::span<const BenefitPlanId> {
  if (owners_.empty()) return {};
  return owners_.front().plans;
}

auto CloudBenefits::grants_streaming() const noexcept -> bool {
  return authorisation_ == Authorisation::granted && !plans().empty();
}

auto CloudBenefits::refusal_needs_wording() const noexcept -> bool {
  return authorisation_ != Authorisation::granted;
}

}  // namespace libchiaking::users
