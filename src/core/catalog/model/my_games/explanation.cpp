#include "libchiaking/detail/catalog/explanation.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

Explanation::Explanation(ProductId product, std::vector<Step> steps) noexcept
    : product_(std::move(product)), steps_(std::move(steps)) {}

auto Explanation::of(ProductId product, std::vector<Step> steps)
    -> Explanation {
  return Explanation{std::move(product), std::move(steps)};
}

auto Explanation::not_listed(ProductId product) -> Explanation {
  return Explanation{std::move(product),
                     {Step{Rule::listed_by_service, Outcome::failed}}};
}

auto Explanation::is_listed() const noexcept -> bool {
  return first_failure() == nullptr;
}

auto Explanation::first_failure() const noexcept -> const Step* {
  const auto failed =
      std::ranges::find(steps_, Outcome::failed, &Step::outcome);
  return failed != steps_.end() ? &*failed : nullptr;
}

auto Explanation::could_not_check_ownership() const noexcept -> bool {
  return std::ranges::any_of(steps_, [](const Step& step) {
    return step.rule == Rule::ownership && step.outcome == Outcome::unchecked;
  });
}

auto Explanation::hide_reason() const noexcept -> std::optional<HideReason> {
  const auto* failed = first_failure();
  return failed != nullptr ? failed->hides : std::nullopt;
}

}  // namespace libchiaking::catalog
