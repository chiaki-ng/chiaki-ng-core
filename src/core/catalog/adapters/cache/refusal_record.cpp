#include "src/core/catalog/adapters/cache/refusal_record.hpp"

#include "src/core/catalog/adapters/cache/cache.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace libchiaking::catalog::cache {

RefusalRecord::RefusalRecord(AccountId account,
                             std::vector<StreamRefusal> refusals) noexcept
    : account_(account), refusals_(std::move(refusals)) {}

auto RefusalRecord::of(AccountId account, std::vector<StreamRefusal> refusals)
    -> std::optional<RefusalRecord> {
  std::ranges::sort(refusals, {}, &StreamRefusal::product_id);
  const bool repeated =
      std::ranges::adjacent_find(refusals, {}, &StreamRefusal::product_id) !=
      refusals.end();
  const bool foreign =
      std::ranges::any_of(refusals, [account](const StreamRefusal& refusal) {
        return refusal.account() != account;
      });
  if (repeated || foreign) return std::nullopt;
  return RefusalRecord{account, std::move(refusals)};
}

auto RefusalRecord::name_of(AccountId account)
    -> std::optional<platform::RecordName> {
  return Cache::user_record(account, kind);
}

auto RefusalRecord::refused(const StreamRefusal& refusal) -> Refused {
  const auto& holding = refusal.circumstances().holding;
  Refused wire{.product = refusal.product_id().text(),
               .kind = refusal.kind(),
               .refused_at_ms = milliseconds_of(refusal.refused_at()),
               .tier = refusal.circumstances().tier,
               .ownership = holding.state()};
  if (holding.id()) wire.ownership_id = holding.id()->text();
  return wire;
}

auto RefusalRecord::body() const -> Body {
  return Body{refusals_ | std::views::transform(&RefusalRecord::refused) |
              std::ranges::to<std::vector>()};
}

auto RefusalRecord::holding_of(const Refused& wire)
    -> std::optional<StreamRefusal::Holding> {
  using State = StreamRefusal::Holding::State;
  switch (wire.ownership) {
    case State::unknown:
      return StreamRefusal::Holding::unknown();
    case State::not_held:
      return StreamRefusal::Holding::not_held();
    case State::held:
      break;
  }
  auto id = stated<OwnershipId>(wire.ownership_id, &OwnershipId::parse);
  if (!id || !*id) return std::nullopt;
  return StreamRefusal::Holding::held_as(std::move(**id));
}

auto RefusalRecord::from_body(const Body& body, AccountId account)
    -> std::optional<RefusalRecord> {
  std::vector<StreamRefusal> refusals;
  refusals.reserve(body.refusals.size());
  for (const auto& wire : body.refusals) {
    auto product = ProductId::parse(wire.product);
    auto holding = holding_of(wire);
    if (!product || !holding) return std::nullopt;
    refusals.emplace_back(
        account, std::move(*product), wire.kind, time_of(wire.refused_at_ms),
        StreamRefusal::Circumstances{wire.tier, std::move(*holding)});
  }
  return of(account, std::move(refusals));
}

}  // namespace libchiaking::catalog::cache
