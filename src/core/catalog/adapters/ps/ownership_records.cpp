#include "src/core/catalog/adapters/ps/ownership_records.hpp"

#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog::ps {

OwnershipRecords::OwnershipRecords(users::PsUser& user, net::http::Client& http,
                                   const platform::ClockProvider& wall,
                                   std::size_t maximum_response_bytes) noexcept
    : user_(user),
      http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto OwnershipRecords::all(PageCount pages) -> Expected<Listed, Unanswered> {
  auto paged = users::CatalogCalls{user_}.library_records(
      http_,
      [&pages](
          int,
          const Expected<std::uint16_t, psn::rest::CommerceError>& outcome) {
        if (outcome) pages.read();
      },
      maximum_response_bytes_);
  if (!paged) return unexpected(Unanswered{paged.error()});
  const auto& entitlements = paged.value().items;
  Listed listed{{}, stamp_.now()};
  for (const auto& entitlement : entitlements)
    if (auto record = record_of(entitlement))
      listed.records.push_back(std::move(*record));
  http_.log(platform::LogSeverity::info,
            "[OWNED] " + std::to_string(entitlements.size()) +
                " entitlements -> " + std::to_string(listed.records.size()) +
                " records");
  http_.log(platform::LogSeverity::info,
            "[OWNED] kinds: " + kinds_of(listed.records));
  return listed;
}

auto OwnershipRecords::kinds_of(std::span<const OwnedRecord> records)
    -> std::string {
  std::map<std::string, std::size_t> counted;
  for (const auto& record : records) {
    const auto& fields = record.fields();
    ++counted[(fields.platform_id.empty() ? "unknown" : fields.platform_id) +
              " " +
              (fields.package_type.empty() ? "untyped" : fields.package_type)];
  }
  std::string kinds;
  for (const auto& [kind, copies] : counted)
    kinds += (kinds.empty() ? "" : ", ") + kind + " x" + std::to_string(copies);
  return kinds;
}

auto OwnershipRecords::lacks_a_concept(
    std::span<const OwnedRecord> records) noexcept -> bool {
  return std::ranges::any_of(records, [](const OwnedRecord& record) {
    return record.fields().concept_id.empty() &&
           text::ieq(record.fields().package_type,
                     psn::rest::LibraryEntitlements::streamed_ps4_games);
  });
}

auto OwnershipRecords::concepts_of_streamed_ps4_copies(PageCount pages)
    -> Expected<std::vector<Library::LearnedConcept>, Unanswered> {
  auto found = users::CatalogCalls{user_}.library_concepts(
      http_, psn::rest::LibraryEntitlements::streamed_ps4_games,
      maximum_response_bytes_, [&pages](std::optional<std::size_t> total) {
        if (total) pages.expect(static_cast<std::uint32_t>(*total));
        pages.read();
      });
  if (!found) return unexpected(Unanswered{found.error()});
  std::vector<Library::LearnedConcept> learned;
  for (const auto& row : found.value().known)
    if (auto grouped = ConceptId::parse(row.concept_id))
      learned.push_back({OwnershipId::parse(row.id),
                         ProductId::parse(row.product_id),
                         std::move(*grouped)});
  http_.log(platform::LogSeverity::info,
            "[OWNED] " + std::to_string(found.value().received) + " of " +
                std::to_string(found.value().total) +
                " streamed PS4 copies read in " +
                std::to_string(found.value().requests) + " requests -> " +
                std::to_string(learned.size()) + " concepts" +
                (found.value().truncated ? " (stopped at the page cap)" : ""));
  return learned;
}

auto OwnershipRecords::one_of(std::span<const OwnershipId> ids)
    -> Expected<std::optional<OwnedRecord>, Unanswered> {
  users::CatalogCalls calls{user_};
  std::vector<OwnershipId> asked;
  for (const auto& id : ids) {
    if (asked.size() >= lookup_limit) break;
    if (std::ranges::find(asked, id) != asked.end()) continue;
    asked.push_back(id);
    auto lookup =
        calls.library_record(http_, id.text(), maximum_response_bytes_);
    if (!lookup) return unexpected(Unanswered{lookup.error()});
    if (!lookup.value().owned) continue;
    if (!lookup.value().entitlement)
      return unexpected(Unanswered{Unanswered::Reason::malformed,
                                   lookup.value().http_status});
    auto record = record_of(*lookup.value().entitlement);
    if (!record)
      return unexpected(Unanswered{Unanswered::Reason::malformed,
                                   lookup.value().http_status});
    return std::optional<OwnedRecord>{std::move(*record)};
  }
  return std::optional<OwnedRecord>{};
}

auto OwnershipRecords::platform_of(const StoreList<Attribute>& attributes)
    -> std::string {
  if (!attributes) return {};
  for (const auto& attribute : *attributes) {
    const auto platform =
        attribute ? attribute->platform_id.view() : std::string_view{};
    if (!text::ieq(platform, "ps5") && !text::ieq(platform, "ps4") &&
        !text::ieq(platform, "ps3"))
      continue;
    std::string lowered{platform};
    std::ranges::transform(lowered, lowered.begin(), text::ascii_lower);
    return lowered;
  }
  return {};
}

auto OwnershipRecords::record_of(const glz::raw_json& entitlement)
    -> std::optional<OwnedRecord> {
  if (!entitlement.str.starts_with('{')) return std::nullopt;
  const auto read = json::ps::read_reply<Granted>(entitlement.str);
  if (!read) return std::nullopt;
  const auto& row = read.value();
  const auto& details = row.game_meta;
  OwnedRecord::Fields fields;
  fields.id = row.id.view();
  fields.product_id = row.product_id.view();
  fields.name = details ? details->name.view() : row.name.view();
  fields.has_game_details = static_cast<bool>(details);
  fields.active = row.active_flag.value_or(false);
  if (row.feature_type)
    if (const auto feature = row.feature_type->integer())
      fields.feature_type = static_cast<int>(*feature);
  if (details) fields.package_type = details->package_type.view();
  fields.sku_type = row.sku_type.view();
  if (fields.sku_type.empty() && details)
    fields.sku_type = details->sku_type.view();
  fields.platform_id = platform_of(row.entitlement_attributes);
  fields.active_date = StoreListing::scalar_text(row.active_date);
  if (row.sku_meta)
    fields.playability_date =
        StoreListing::scalar_text(row.sku_meta->playability_date);
  fields.preorder = row.preorder_flag.value_or(false);
  const auto icon = details ? details->icon_url.view() : std::string_view{};
  if (!icon.empty())
    fields.cover_url = icon.substr(0, icon_url_limit);
  else if (details)
    fields.cover_url =
        StoreListing::cover(details->images, details->image_url.view());
  if (fields.cover_url.empty())
    fields.cover_url = StoreListing::cover(row.images, row.image_url.view());
  fields.sku_id = row.sku_id.view();
  auto grouped = StoreListing::concept_of(row.concept_id);
  if (!grouped) grouped = StoreListing::concept_of(row.snake_concept_id);
  if (!grouped && details)
    grouped = StoreListing::concept_of(details->concept_id);
  if (grouped) fields.concept_id = grouped->text();
  return OwnedRecord{std::move(fields)};
}

}  // namespace libchiaking::catalog::ps
