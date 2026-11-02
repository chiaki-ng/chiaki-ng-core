#include "src/core/catalog/adapters/cache/refusal_memory.hpp"

#include "src/core/catalog/adapters/cache/refusal_record.hpp"

#include <utility>

namespace libchiaking::catalog {

RefusalMemory::RefusalMemory(cache::Cache& cache,
                             platform::ThreadingProvider& threading)
    : cache_(cache), mutex_(threading) {}

auto RefusalMemory::remember(AccountId account, const ProductId& product,
                             StreamRefusal::Kind kind,
                             const Circumstances& current)
    -> Expected<StreamRefusal, Unanswered> {
  const platform::Lock lock{mutex_};
  const StreamRefusal refusal{account, product, kind, cache_.now(), current};
  const auto loaded = held(account);
  auto& memory = accounts_[account.value()];
  memory.refusals.insert_or_assign(product, refusal);
  memory.revision = ++changes_;
  if (!loaded) return unexpected(loaded.error());
  if (const auto saved = save(account, memory); !saved)
    return unexpected(saved.error());
  return refusal;
}

auto RefusalMemory::standing(AccountId account, std::optional<users::Tier> tier)
    -> Expected<Standing, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  auto& memory = *loaded.value();
  const auto now = cache_.now();
  clear(account, memory, [&](const StreamRefusal& refusal) {
    return !refusal.still_stands(now, {tier, refusal.circumstances().holding});
  });
  return all_of(memory);
}

auto RefusalMemory::standing_while_membership_is_unknown(AccountId account)
    -> Expected<Standing, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  auto& memory = *loaded.value();
  const auto now = cache_.now();
  clear(account, memory, [&](const StreamRefusal& refusal) {
    return !refusal.still_stands(now, refusal.circumstances());
  });
  return all_of(memory);
}

auto RefusalMemory::forget_where_ownership_changed(
    AccountId account,
    const std::function<StreamRefusal::Holding(const ProductId&)>& held_now)
    -> Expected<std::uint64_t, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  auto& memory = *loaded.value();
  clear(account, memory, [&](const StreamRefusal& refusal) {
    return refusal.circumstances().holding.has_changed_to(
        held_now(refusal.product_id()));
  });
  return memory.revision;
}

auto RefusalMemory::refusal_of(AccountId account, const ProductId& product,
                               const Circumstances& current)
    -> Expected<std::optional<StreamRefusal>, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  auto& memory = *loaded.value();
  const auto now = cache_.now();
  clear(account, memory, [&](const StreamRefusal& refusal) {
    const auto& holding = refusal.product_id() == product
                              ? current.holding
                              : refusal.circumstances().holding;
    return !refusal.still_stands(now, {current.tier, holding});
  });
  const auto found = memory.refusals.find(product);
  if (found == memory.refusals.end()) return std::optional<StreamRefusal>{};
  return std::optional<StreamRefusal>{found->second};
}

auto RefusalMemory::revision(AccountId account) -> std::uint64_t {
  const platform::Lock lock{mutex_};
  const auto found = accounts_.find(account.value());
  return found == accounts_.end() ? 0 : found->second.revision;
}

auto RefusalMemory::forget(AccountId account) -> Expected<void, Unanswered> {
  const platform::Lock lock{mutex_};
  accounts_.erase(account.value());
  ++changes_;
  return cache_.erase<cache::RefusalRecord>(account);
}

auto RefusalMemory::forget_everyone() -> Expected<void, Unanswered> {
  const platform::Lock lock{mutex_};
  accounts_.clear();
  ++changes_;
  return cache_.erase_for_every_account(cache::RefusalRecord::kind);
}

auto RefusalMemory::held(AccountId account) -> Expected<Held*, Unanswered> {
  auto& memory = accounts_[account.value()];
  if (memory.loaded) return &memory;
  const auto record = cache_.read<cache::RefusalRecord>(account);
  if (!record) return unexpected(record.error());
  const bool unsaved = !memory.refusals.empty();
  if (record.value())
    for (const auto& refusal : record.value()->refusals())
      memory.refusals.try_emplace(refusal.product_id(), refusal);
  memory.revision = ++changes_;
  memory.loaded = true;
  if (unsaved) (void)save(account, memory);
  return &memory;
}

auto RefusalMemory::save(AccountId account, const Held& held)
    -> Expected<void, Unanswered> {
  if (held.refusals.empty()) return cache_.erase<cache::RefusalRecord>(account);
  std::vector<StreamRefusal> refusals;
  refusals.reserve(held.refusals.size());
  for (const auto& [product, refusal] : held.refusals)
    refusals.push_back(refusal);
  const auto record = cache::RefusalRecord::of(account, std::move(refusals));
  if (!record) return unexpected(Unanswered{Unanswered::Reason::storage});
  return cache_.write(*record);
}

template <class Fallen>
void RefusalMemory::clear(AccountId account, Held& held, const Fallen& fallen) {
  const auto cleared = std::erase_if(
      held.refusals, [&](const auto& entry) { return fallen(entry.second); });
  if (cleared == 0) return;
  held.revision = ++changes_;
  (void)save(account, held);
}

auto RefusalMemory::all_of(const Held& held) -> Standing {
  Standing standing{{}, held.revision};
  standing.refusals.reserve(held.refusals.size());
  for (const auto& [product, refusal] : held.refusals)
    standing.refusals.push_back(refusal);
  return standing;
}

}  // namespace libchiaking::catalog
