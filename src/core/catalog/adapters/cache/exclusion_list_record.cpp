#include "src/core/catalog/adapters/cache/exclusion_list_record.hpp"

#include <utility>

namespace libchiaking::catalog::cache {

ExclusionListRecord::ExclusionListRecord(
    cloud_stream::ExclusionList list) noexcept
    : list_(std::move(list)) {}

auto ExclusionListRecord::name_of(const Everyone&)
    -> std::optional<platform::RecordName> {
  return Cache::shared_record(kind, scope);
}

auto ExclusionListRecord::body() const -> Body {
  return Body{.fetched_at_ms = milliseconds_of(list_.fetched_at()),
              .excluded_title_ids = texts_of(list_.excluded())};
}

auto ExclusionListRecord::from_body(const Body& body, const Everyone&)
    -> std::optional<ExclusionListRecord> {
  auto excluded = every<TitleId>(
      body.excluded_title_ids,
      [](const std::string& text) { return TitleId::parse(text); });
  if (!excluded) return std::nullopt;
  return ExclusionListRecord{cloud_stream::ExclusionList{
      std::move(*excluded), time_of(body.fetched_at_ms)}};
}

}  // namespace libchiaking::catalog::cache
