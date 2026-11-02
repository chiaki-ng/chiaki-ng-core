#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"

#include <string>
#include <utility>

namespace libchiaking::catalog::cache {

StreamableListRecord::StreamableListRecord(StreamableList list) noexcept
    : list_(std::move(list)) {}

auto StreamableListRecord::of(StreamableList list)
    -> std::optional<StreamableListRecord> {
  if (!list.is_complete() || list.origin() == ListOrigin::public_store)
    return std::nullopt;
  return StreamableListRecord{std::move(list)};
}

auto StreamableListRecord::name_of(const StoreLocale& region)
    -> std::optional<platform::RecordName> {
  const auto lowered = region.lowercase();
  const auto language =
      std::string_view{lowered}.substr(0, region.language().size());
  const auto country =
      std::string_view{lowered}.substr(region.language().size() + 1);
  return Cache::shared_record(
      kind, std::string{country} + "-" + std::string{language});
}

auto StreamableListRecord::body() const -> Body {
  Body body{.settled_locale = list_.settled_locale().text(),
            .origin = list_.origin(),
            .complete = list_.is_complete(),
            .fetched_at_ms = milliseconds_of(list_.fetched_at()),
            .titles = entries(list_.titles()),
            .included_but_not_streamable =
                entries(list_.included_but_not_streamable()),
            .marked_unstreamable = texts_of(list_.marked_unstreamable())};
  if (const auto& copies = list_.streaming_copies()) {
    body.streaming_copies_asked = static_cast<std::int64_t>(copies->asked);
    body.streaming_copies_answered =
        static_cast<std::int64_t>(copies->answered);
  }
  return body;
}

auto StreamableListRecord::from_body(const Body& body,
                                     const StoreLocale& region)
    -> std::optional<StreamableListRecord> {
  const auto read = [](const Listed& wire) { return entry(wire); };
  auto settled = StoreLocale::parse(body.settled_locale);
  auto titles = every_read<StreamableList::Entry>(body.titles, read);
  auto not_streamable =
      every_read<StreamableList::Entry>(body.included_but_not_streamable, read);
  auto marked = every<TitleNumber>(body.marked_unstreamable,
                                   &StreamableListRecord::marked_number);
  if (!settled || !titles || !not_streamable || !marked) return std::nullopt;
  auto list = StreamableList::of(StreamableList::Facts{
      region, std::move(*settled), body.origin, body.complete,
      time_of(body.fetched_at_ms), std::move(*titles),
      std::move(*not_streamable), std::move(*marked), copies_of(body)});
  if (!list) return std::nullopt;
  return of(std::move(list.value()));
}

auto StreamableListRecord::marked_number(const std::string& text)
    -> std::optional<TitleNumber> {
  auto number = TitleNumber::from(text);
  if (!number || number->text() != text) return std::nullopt;
  return number;
}

auto StreamableListRecord::copies_of(const Body& body)
    -> std::optional<StreamableList::CopiesChecked> {
  const auto asked = body.streaming_copies_asked;
  const auto answered = body.streaming_copies_answered;
  if (!asked || !answered || *asked < 0 || *answered < 0 || *answered > *asked)
    return std::nullopt;
  return StreamableList::CopiesChecked{static_cast<std::size_t>(*asked),
                                       static_cast<std::size_t>(*answered)};
}

auto StreamableListRecord::entries(
    std::span<const StreamableList::Entry> listed) -> std::vector<Listed> {
  std::vector<Listed> wires;
  wires.reserve(listed.size());
  for (const auto& one : listed) wires.push_back(entry(one));
  return wires;
}

auto StreamableListRecord::entry(const StreamableList::Entry& listed)
    -> Listed {
  const auto& title = listed.title;
  Listed wire{.product = title.product_id().text(),
              .name = title.name(),
              .platform = title.platform(),
              .service = title.service(),
              .streamability = title.streamability(),
              .cover = title.cover_url(),
              .landscape_cover = title.landscape_cover_url(),
              .aliases = texts_of(title.aliases()),
              .inclusion_source = listed.inclusion.source(),
              .inclusion_tier = listed.inclusion.lowest_tier()};
  if (title.concept_id()) wire.concept_id = title.concept_id()->text();
  if (title.concept_page()) {
    wire.concept_page = title.concept_page()->url();
    wire.concept_page_source = title.concept_page()->source();
  }
  return wire;
}

auto StreamableListRecord::page_of(const Listed& wire)
    -> std::optional<std::optional<StorePage>> {
  if (!wire.concept_page) return std::optional<StorePage>{};
  auto page = wire.concept_page_source
                  ? StorePage::at(*wire.concept_page, *wire.concept_page_source)
                  : std::nullopt;
  if (!page) return std::nullopt;
  return std::optional<StorePage>{std::move(*page)};
}

auto StreamableListRecord::inclusion_of(const Listed& wire)
    -> std::optional<PlusInclusion> {
  if (!wire.inclusion_tier)
    return PlusInclusion::not_included(wire.inclusion_source);
  return PlusInclusion::from_tier(*wire.inclusion_tier, wire.inclusion_source);
}

auto StreamableListRecord::entry(const Listed& wire)
    -> std::optional<StreamableList::Entry> {
  auto product = ProductId::parse(wire.product);
  auto concept_id = stated<ConceptId>(wire.concept_id, &ConceptId::parse);
  auto page = page_of(wire);
  auto aliases = every<ProductId>(wire.aliases, &ProductId::parse);
  const auto inclusion = inclusion_of(wire);
  if (!product || !concept_id || !page || !aliases || !inclusion)
    return std::nullopt;
  auto title = Title::from(Title::Facts{
      std::move(*product), wire.name, wire.platform, wire.service,
      wire.streamability, std::move(*concept_id), wire.cover,
      wire.landscape_cover, std::move(*page), std::move(*aliases)});
  if (!title) return std::nullopt;
  return StreamableList::Entry{std::move(title.value()), *inclusion};
}

}  // namespace libchiaking::catalog::cache
