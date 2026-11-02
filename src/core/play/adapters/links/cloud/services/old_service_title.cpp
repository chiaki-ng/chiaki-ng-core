#include "src/core/play/adapters/links/cloud/services/old_service_title.hpp"

#include "src/core/psn/auth/gk_ids.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <vector>

namespace libchiaking::play {

auto OldServiceTitle::text_of(const Text& value) -> std::string_view {
  return value ? std::string_view{*value} : std::string_view{};
}

auto OldServiceTitle::upper(std::string_view value) -> std::string {
  std::string result{value};
  std::ranges::transform(result, result.begin(), text::ascii_upper);
  return result;
}

auto OldServiceTitle::select(const Container& container,
                             std::string_view identifier)
    -> std::optional<Candidate> {
  std::vector<const Tolerant<Sku>*> skus;
  skus.push_back(&container.default_sku);
  if (container.skus)
    for (const auto& value : *container.skus) skus.push_back(&value);
  std::string_view title;
  const auto dash = identifier.find('-');
  if (dash != std::string_view::npos) {
    title = identifier.substr(dash + 1);
    title = title.substr(0, title.find('_'));
    if (title.size() >= 64) title = {};
  }
  for (int pass = 0; pass < 5; ++pass) {
    for (const auto* sku : skus) {
      if (!*sku || !(*sku)->entitlements) continue;
      for (const auto& entry : *(*sku)->entitlements) {
        if (!entry) continue;
        const auto id = text_of(entry->id);
        if (id.empty()) continue;
        const auto license =
            entry->license_type ? entry->license_type->integer() : std::nullopt;
        const auto package = text_of(entry->package_type);
        if (pass == 0 && license != 4) continue;
        if (pass > 0 && !package.ends_with(pass < 3 ? "GS" : "GD")) continue;
        if ((pass == 1 || pass == 3) && !title.empty() &&
            id.find(title) == std::string_view::npos)
          continue;
        return Candidate{id, text_of((*sku)->id)};
      }
    }
  }
  return std::nullopt;
}

auto OldServiceTitle::playable_platform(const Container& container)
    -> psn::StreamingPlatform {
  const auto* platforms = &container.playable_platform;
  if (!*platforms && container.metadata &&
      container.metadata->playable_platform)
    platforms = &container.metadata->playable_platform->values;
  bool ps5 = false, ps4 = false, ps3 = false;
  if (*platforms) {
    for (const auto& value : **platforms) {
      const auto name =
          upper(value ? std::string_view{*value} : std::string_view{});
      if (name.find("PS5") != std::string::npos)
        ps5 = true;
      else if (name.find("PS4") != std::string::npos)
        ps4 = true;
      else if (name.find("PS3") != std::string::npos)
        ps3 = true;
    }
  }
  return ps5   ? psn::StreamingPlatform::ps5
         : ps4 ? psn::StreamingPlatform::ps4
         : ps3 ? psn::StreamingPlatform::ps3
               : psn::StreamingPlatform::ps4;
}

auto OldServiceTitle::plan_cover(const Container& container,
                                 std::string_view sku) -> PlanCover {
  if (sku.empty() || !container.offers || !container.offers->sku_relevancy)
    return PlanCover::unstated;
  const auto& stated = *container.offers->sku_relevancy;
  const auto about =
      std::ranges::find_if(stated, [&](const Tolerant<Relevancy>& entry) {
        return entry && text_of(entry->sku_id) == sku;
      });
  if (about == stated.end() || !(*about)->offer_availability)
    return PlanCover::unstated;
  return (*about)->offer_availability->empty() ? PlanCover::absent
                                               : PlanCover::covered;
}

OldServiceTitle::OldServiceTitle(std::string_view entitlement,
                                 psn::StreamingPlatform platform)
    : entitlement_(entitlement.substr(0, psn::gk_id_max_length)),
      platform_(platform) {}

auto OldServiceTitle::decode(const Reply& reply, std::string_view identifier)
    -> std::optional<OldServiceTitle> {
  if (!reply) return std::nullopt;
  const auto& container = *reply;
  const auto selected = select(container, identifier);
  if (!selected) return std::nullopt;
  OldServiceTitle title{selected->entitlement, playable_platform(container)};
  title.replace_sku(selected->sku);
  title.plan_cover_ = plan_cover(container, selected->sku);
  return title;
}

auto OldServiceTitle::is_legacy(std::string_view identifier) -> bool {
  const auto dash = identifier.find('-');
  if (dash == std::string_view::npos) return true;
  const auto title = identifier.substr(dash + 1);
  return !title.starts_with("CUSA") && !title.starts_with("PPSA");
}

auto OldServiceTitle::checkout_sku() const noexcept -> std::string_view {
  return sku_.empty() ? entitlement_ : sku_;
}

void OldServiceTitle::replace_sku(std::string_view sku) {
  sku_ = sku.substr(0, sku_limit);
}

}  // namespace libchiaking::play
