#include "src/core/catalog/adapters/cache/library_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"

#include <ranges>
#include <utility>

namespace libchiaking::catalog::cache {

LibraryRecord::LibraryRecord(Library library,
                             std::optional<OwnedPs5Games> ps5) noexcept
    : library_(std::move(library)), ps5_(std::move(ps5)) {}

auto LibraryRecord::name_of(AccountId account)
    -> std::optional<platform::RecordName> {
  return Cache::user_record(account, kind);
}

auto LibraryRecord::body() const -> Body {
  Body body{.fetched_at_ms = milliseconds_of(library_.fetched_at())};
  for (const auto& title : library_.titles())
    body.titles.push_back(owned(title));
  for (const auto& one : library_.bundles())
    body.bundles.push_back({one.product.text(), texts_of(one.siblings)});
  if (!library_.copy_concepts().empty())
    body.copy_concepts =
        library_.copy_concepts() |
        std::views::transform([](const Library::CopyConcept& copy) {
          return Grouped{copy.id.text(), copy.grouped_under.text()};
        }) |
        std::ranges::to<std::vector>();
  body.ps5_ownership = library_.ps5_ownership();
  if (ps5_) body.ps5_games = ps5_part(*ps5_);
  return body;
}

auto LibraryRecord::from_body(const Body& body, AccountId account)
    -> std::optional<LibraryRecord> {
  auto titles = every_read<OwnedTitle>(
      body.titles, [](const Owned& wire) { return owned(wire); });
  auto bundles =
      every_read<Library::Bundle>(body.bundles, &LibraryRecord::bundle);
  if (!titles || !bundles) return std::nullopt;
  auto library = Library::of(account, std::move(*titles), std::move(*bundles),
                             time_of(body.fetched_at_ms), body.ps5_ownership);
  if (!library) return std::nullopt;
  std::optional<OwnedPs5Games> ps5;
  if (body.ps5_games) {
    ps5 = ps5_part(*body.ps5_games);
    if (!ps5) return std::nullopt;
  }
  if (!body.copy_concepts)
    return LibraryRecord{std::move(library.value()), std::move(ps5)};
  const auto learned_concepts = every_read<Library::LearnedConcept>(
      *body.copy_concepts, &LibraryRecord::learned);
  if (!learned_concepts) return std::nullopt;
  return LibraryRecord{library.value().with_learned_concepts(*learned_concepts),
                       std::move(ps5)};
}

auto LibraryRecord::owned(const OwnedTitle& title) -> Owned {
  Owned wire{.id = title.id().text(),
             .name = title.name(),
             .platform = title.platform(),
             .service = title.service(),
             .held_as = title.held_as(),
             .cover = title.cover_url(),
             .streams = title.streams()};
  if (const auto time = title.release().time())
    wire.release_at_ms = milliseconds_of(*time);
  if (title.release().is_unannounced()) wire.release_unannounced = true;
  if (title.product_id()) wire.product = title.product_id()->text();
  if (title.concept_id()) wire.concept_id = title.concept_id()->text();
  return wire;
}

auto LibraryRecord::release_of(const Owned& wire)
    -> std::optional<ReleaseDate> {
  if (wire.release_unannounced)
    return *wire.release_unannounced && !wire.release_at_ms
               ? std::optional{ReleaseDate::unannounced()}
               : std::nullopt;
  if (!wire.release_at_ms) return ReleaseDate::unrestricted();
  const auto release = ReleaseDate::at(time_of(*wire.release_at_ms));
  if (release.is_unrestricted()) return std::nullopt;
  return release;
}

auto LibraryRecord::owned(const Owned& wire) -> std::optional<OwnedTitle> {
  auto id = OwnershipId::parse(wire.id);
  const auto release = release_of(wire);
  auto product = stated<ProductId>(wire.product, &ProductId::parse);
  auto concept_id = stated<ConceptId>(wire.concept_id, &ConceptId::parse);
  if (!id || !release || !product || !concept_id) return std::nullopt;
  auto title = OwnedTitle::from(
      OwnedTitle::Facts{std::move(*id), wire.name, wire.platform, wire.service,
                        wire.held_as, *release, std::move(*product),
                        std::move(*concept_id), wire.cover, wire.streams});
  if (!title) return std::nullopt;
  return std::move(title.value());
}

auto LibraryRecord::bundle(const Bundled& wire)
    -> std::optional<Library::Bundle> {
  auto product = ProductId::parse(wire.product);
  auto siblings = every<ProductId>(wire.siblings, &ProductId::parse);
  if (!product || !siblings) return std::nullopt;
  return Library::Bundle{std::move(*product), std::move(*siblings)};
}

auto LibraryRecord::learned(const Grouped& wire)
    -> std::optional<Library::LearnedConcept> {
  auto id = OwnershipId::parse(wire.id);
  auto grouped = ConceptId::parse(wire.concept_id);
  if (!id || !grouped) return std::nullopt;
  return Library::LearnedConcept{std::move(id), std::nullopt,
                                 std::move(*grouped)};
}

auto LibraryRecord::ps5_part(const OwnedPs5Games& games) -> Ps5Games {
  return Ps5Games{
      .fetched_at_ms = milliseconds_of(games.fetched_at()),
      .state = games.state(),
      .records_read = static_cast<std::int64_t>(games.scan().records_read),
      .revision_restarts = games.scan().revision_restarts,
      .records = games.records() |
                 std::views::transform([](const Ps5Entitlement& record) {
                   return ps5_record(record);
                 }) |
                 std::ranges::to<std::vector>()};
}

auto LibraryRecord::ps5_part(const Ps5Games& wire)
    -> std::optional<OwnedPs5Games> {
  auto records = every_read<Ps5Entitlement>(
      wire.records, [](const Ps5Record& record) { return ps5_record(record); });
  if (!records || wire.records_read < 0) return std::nullopt;
  return OwnedPs5Games{std::move(*records),
                       wire.state,
                       {static_cast<std::size_t>(wire.records_read),
                        static_cast<int>(wire.revision_restarts)},
                       time_of(wire.fetched_at_ms)};
}

auto LibraryRecord::ps5_record(const Ps5Entitlement& record) -> Ps5Record {
  const auto& facts = record.facts();
  const auto text = [](const auto& id) {
    return id ? std::optional<std::string>{id->text()} : std::nullopt;
  };
  return Ps5Record{.id = facts.id.text(),
                   .product = text(facts.product),
                   .sku = text(facts.sku),
                   .title = text(facts.title),
                   .concept_id = text(facts.concept_id),
                   .name = facts.name,
                   .cover = facts.cover_url,
                   .active = facts.active,
                   .entitlement_type = facts.entitlement_type,
                   .feature_type = facts.feature_type,
                   .package_type = facts.package_type,
                   .streaming_supported = facts.streaming_supported,
                   .play_time_used_up = facts.play_time_used_up,
                   .preorder_core = facts.preorder_core,
                   .preorder_placeholder = facts.preorder_placeholder,
                   .playable_from = facts.playable_from,
                   .active_date = facts.active_date};
}

auto LibraryRecord::ps5_record(const Ps5Record& wire)
    -> std::optional<Ps5Entitlement> {
  auto id = OwnershipId::parse(wire.id);
  auto product = stated<ProductId>(wire.product, &ProductId::parse);
  auto sku = stated<SkuId>(wire.sku, &SkuId::parse);
  auto title = stated<TitleId>(wire.title, &TitleId::parse);
  auto concept_id = stated<ConceptId>(wire.concept_id, &ConceptId::parse);
  if (!id || !product || !sku || !title || !concept_id) return std::nullopt;
  return Ps5Entitlement{
      Ps5Entitlement::Facts{.id = std::move(*id),
                            .product = std::move(*product),
                            .sku = std::move(*sku),
                            .title = std::move(*title),
                            .concept_id = std::move(*concept_id),
                            .name = wire.name,
                            .cover_url = wire.cover,
                            .active = wire.active,
                            .entitlement_type = wire.entitlement_type,
                            .feature_type = wire.feature_type,
                            .package_type = wire.package_type,
                            .streaming_supported = wire.streaming_supported,
                            .play_time_used_up = wire.play_time_used_up,
                            .preorder_core = wire.preorder_core,
                            .preorder_placeholder = wire.preorder_placeholder,
                            .playable_from = wire.playable_from,
                            .active_date = wire.active_date}};
}

}  // namespace libchiaking::catalog::cache
