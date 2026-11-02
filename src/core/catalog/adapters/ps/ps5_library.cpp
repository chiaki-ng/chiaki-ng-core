#include "src/core/catalog/adapters/ps/ps5_library.hpp"

#include "src/core/users/ports/catalog_calls.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <string_view>
#include <utility>

namespace libchiaking::catalog::ps {

Ps5Library::Ps5Library(users::PsUser& user, net::http::Client& http,
                       const platform::ClockProvider& wall,
                       std::size_t maximum_response_bytes) noexcept
    : user_(user),
      http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto Ps5Library::scan(PageCount pages) -> Expected<Scanned, Unanswered> {
  auto found = users::CatalogCalls{user_}.library_ps5_games(
      http_, maximum_response_bytes_,
      [&pages](std::optional<std::size_t> total) {
        if (total) pages.expect(static_cast<std::uint32_t>(*total));
        pages.read();
      });
  if (!found) return unexpected(Unanswered{found.error()});
  const auto& read = found.value();
  Scanned scanned{.ownership = read.revision_unsettled || read.truncated
                                   ? LibraryState::partial
                                   : LibraryState::fetched,
                  .read = read.received,
                  .revision_restarts = read.revision_restarts,
                  .fetched_at = stamp_.now()};
  for (const auto& game : read.entitlements)
    if (auto record = record_of(game))
      scanned.records.push_back(std::move(*record));
  note(scanned, read);
  return scanned;
}

auto Ps5Library::one(const OwnershipId& id)
    -> Expected<std::optional<Ps5Entitlement>, Unanswered> {
  auto lookup = users::CatalogCalls{user_}.library_ps5_game(
      http_, id.text(), maximum_response_bytes_);
  if (!lookup) return unexpected(Unanswered{lookup.error()});
  if (!lookup.value()) return std::optional<Ps5Entitlement>{};
  auto record = record_of(*lookup.value());
  if (!record) return unexpected(Unanswered{Unanswered::Reason::malformed});
  return std::optional<Ps5Entitlement>{std::move(*record)};
}

auto Ps5Library::one_of(std::span<const OwnershipId> ids)
    -> Expected<std::optional<Ps5Entitlement>, Unanswered> {
  std::vector<OwnershipId> asked;
  for (const auto& id : ids) {
    if (asked.size() >= lookup_limit) break;
    if (std::ranges::find(asked, id) != asked.end()) continue;
    asked.push_back(id);
    auto found = one(id);
    if (!found || found.value()) return found;
  }
  return std::optional<Ps5Entitlement>{};
}

auto Ps5Library::asks_about(const OwnershipId& id) -> bool {
  const auto number = id.title_number();
  return !number || number->platform() == Platform::ps5;
}

auto Ps5Library::record_of(const psn::rest::GameEntitlement& game)
    -> std::optional<Ps5Entitlement> {
  auto id = OwnershipId::parse(game.id);
  if (!id) return std::nullopt;
  Ps5Entitlement::Facts facts{.id = std::move(*id)};
  facts.product = ProductId::parse(game.product_id);
  facts.sku = SkuId::parse(game.sku_id);
  facts.title = TitleId::parse(game.title_id);
  facts.concept_id = ConceptId::parse(game.concept_id);
  facts.name = name_of(game);
  facts.cover_url = cover_of(game);
  facts.active = game.active;
  facts.entitlement_type = game.entitlement_type;
  facts.feature_type = game.feature_type;
  facts.package_type = game.package_type;
  facts.streaming_supported = game.streaming_supported;
  facts.play_time_used_up = game.play_time_used_up;
  facts.preorder_core = game.preorder_core == true;
  facts.preorder_placeholder = game.preorder_placeholder == true;
  facts.playable_from = game.playability_date;
  facts.active_date = game.active_date;
  return Ps5Entitlement{std::move(facts)};
}

auto Ps5Library::name_of(const psn::rest::GameEntitlement& game)
    -> std::string {
  for (const auto* name :
       {&game.game_name, &game.title_name, &game.concept_name})
    if (!name->empty()) return *name;
  return {};
}

auto Ps5Library::cover_of(const psn::rest::GameEntitlement& game)
    -> std::string {
  for (const auto* url :
       {&game.concept_icon_url, &game.game_icon_url, &game.product_image_url})
    if (!url->empty()) return url->substr(0, image_url_limit);
  return {};
}

void Ps5Library::note(const Scanned& scanned,
                      const psn::rest::GameEntitlements& found) const {
  std::map<NotAGame, std::size_t> dropped;
  std::size_t games = 0;
  for (const auto& record : scanned.records) {
    if (const auto why = record.why_not_a_game()) {
      ++dropped[*why];
      note_dropped_trial(record, *why);
      continue;
    }
    ++games;
    if (record.is_named_like_an_extra())
      http_.log(platform::LogSeverity::info,
                "[OWNED] the PS5 game " + record.id().text() +
                    " is named like a demo, trial or extra; PS's record "
                    "decides it is a game");
  }
  std::string reasons;
  for (const auto& [why, copies] : dropped)
    reasons += ", " + std::to_string(copies) + " not a game (reason " +
               std::to_string(static_cast<int>(why)) + ")";
  http_.log(
      platform::LogSeverity::info,
      "[OWNED] " + std::to_string(found.received) + " of " +
          std::to_string(found.total) + " PS5 entitlements read in " +
          std::to_string(found.requests) + " requests -> " +
          std::to_string(games) + " games" + reasons +
          (found.revision_restarts != 0
               ? " after " + std::to_string(found.revision_restarts) +
                     " revision restarts"
               : std::string{}) +
          (found.revision_unsettled ? " (the revision kept changing; partial)"
                                    : "") +
          (found.truncated ? " (stopped at the page cap; partial)" : ""));
}

void Ps5Library::note_dropped_trial(const Ps5Entitlement& record,
                                    NotAGame why) const {
  const auto& facts = record.facts();
  if (facts.feature_type != Ps5Entitlement::trial_feature &&
      facts.feature_type != Ps5Entitlement::play_first_trial_feature)
    return;
  http_.log(
      platform::LogSeverity::info,
      std::format(
          "[TRIAL] the PS5 scan dropped trial {} \"{}\" with feature type {} "
          "(reason {}): active {}, streaming {}, play time used up {}, "
          "active date {}",
          record.id().text(), facts.name, *facts.feature_type,
          static_cast<int>(why), flag_text(facts.active),
          flag_text(facts.streaming_supported),
          flag_text(facts.play_time_used_up),
          facts.active_date.value_or("absent")));
}

auto Ps5Library::flag_text(const std::optional<bool>& flag) noexcept
    -> std::string_view {
  if (!flag) return "absent";
  return *flag ? "true" : "false";
}

}  // namespace libchiaking::catalog::ps
