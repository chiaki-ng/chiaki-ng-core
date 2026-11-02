#include "src/core/catalog/model/cloud_stream/exclusions.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::catalog::cloud_stream {

ExclusionList::ExclusionList(std::vector<TitleId> excluded,
                             Time fetched_at) noexcept
    : excluded_(std::move(excluded)), fetched_at_(fetched_at) {}

auto ExclusionList::is_fresh_at(Time now) const noexcept -> bool {
  return now >= fetched_at_ && now - fetched_at_ < fresh_for;
}

auto ExclusionList::match(const std::optional<TitleId>& title,
                          const ProductId& product) const
    -> std::optional<Match> {
  if (title) {
    const auto found = std::ranges::find(excluded_, *title);
    if (found == excluded_.end()) return std::nullopt;
    return Match{Rule::exact_title_id, *found};
  }
  const auto found =
      std::ranges::find_if(excluded_, [&product](const TitleId& excluded) {
        return product.text().find(excluded.text()) != std::string::npos;
      });
  if (found == excluded_.end()) return std::nullopt;
  return Match{Rule::product_id_contains, *found};
}

ExclusionVerdict::ExclusionVerdict(Outcome outcome) noexcept
    : outcome_(std::move(outcome)) {}

auto ExclusionVerdict::judged(const Expected<ExclusionList, Unanswered>& list,
                              const std::optional<TitleId>& title,
                              const ProductId& product) -> ExclusionVerdict {
  if (!list) return ExclusionVerdict{Unavailable{list.error()}};
  if (auto found = list.value().match(title, product))
    return ExclusionVerdict{
        Excluded{std::move(*found), list.value().fetched_at()}};
  return ExclusionVerdict{NotExcluded{list.value().fetched_at()}};
}

auto ExclusionVerdict::is_excluded() const noexcept -> bool {
  return std::holds_alternative<Excluded>(outcome_);
}

auto ExclusionVerdict::is_unavailable() const noexcept -> bool {
  return std::holds_alternative<Unavailable>(outcome_);
}

auto ExclusionVerdict::refuses_launch() const noexcept -> bool {
  return !std::holds_alternative<NotExcluded>(outcome_);
}

auto ExclusionVerdict::match() const -> std::optional<ExclusionList::Match> {
  if (const auto* excluded = std::get_if<Excluded>(&outcome_))
    return excluded->match;
  return std::nullopt;
}

auto ExclusionVerdict::cause() const noexcept -> std::optional<Unanswered> {
  if (const auto* unavailable = std::get_if<Unavailable>(&outcome_))
    return unavailable->cause;
  return std::nullopt;
}

auto ExclusionVerdict::snapshot_at() const noexcept -> std::optional<Time> {
  if (const auto* excluded = std::get_if<Excluded>(&outcome_))
    return excluded->snapshot_at;
  if (const auto* clear = std::get_if<NotExcluded>(&outcome_))
    return clear->snapshot_at;
  return std::nullopt;
}

auto ExclusionVerdict::refusal() const -> std::optional<LaunchRefusal> {
  if (is_excluded())
    return LaunchRefusal::not_streamable(std::string{excluded_notice});
  const auto unavailable = cause();
  if (!unavailable) return std::nullopt;
  if (unavailable->is_cancelled())
    return LaunchRefusal::check_unavailable(
        unavailable->as(LaunchCheckReason::unavailable));
  return LaunchRefusal::check_unavailable(
      LaunchCheckFailure{LaunchCheckReason::restriction_list_unavailable,
                         unavailable->http_status()});
}

}  // namespace libchiaking::catalog::cloud_stream
