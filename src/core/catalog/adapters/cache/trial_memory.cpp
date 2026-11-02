#include "src/core/catalog/adapters/cache/trial_memory.hpp"
#include "src/core/catalog/adapters/cache/cache.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace libchiaking::catalog::cache {

UsedTrialRecord::UsedTrialRecord(AccountId account,
                                 std::vector<ProductId> products) noexcept
    : account_(account), products_(std::move(products)) {}

auto UsedTrialRecord::of(AccountId account, std::vector<ProductId> products)
    -> UsedTrialRecord {
  std::ranges::sort(products);
  const auto repeated = std::ranges::unique(products);
  products.erase(repeated.begin(), repeated.end());
  return UsedTrialRecord{account, std::move(products)};
}

auto UsedTrialRecord::name_of(AccountId account)
    -> std::optional<platform::RecordName> {
  return Cache::user_record(account, kind);
}

auto UsedTrialRecord::body() const -> Body {
  return Body{products_ | std::views::transform(&ProductId::text) |
              std::ranges::to<std::vector<std::string>>()};
}

auto UsedTrialRecord::from_body(const Body& body, AccountId account)
    -> std::optional<UsedTrialRecord> {
  std::vector<ProductId> products;
  products.reserve(body.products.size());
  for (const auto& text : body.products) {
    auto product = ProductId::parse(text);
    if (!product) return std::nullopt;
    products.push_back(std::move(*product));
  }
  return of(account, std::move(products));
}

}  // namespace libchiaking::catalog::cache

namespace libchiaking::catalog {

TrialMemory::TrialMemory(cache::Cache& cache,
                         platform::ThreadingProvider& threading)
    : cache_(cache), mutex_(threading) {}

auto TrialMemory::note(AccountId account, const ProductId& product,
                       bool offered) -> Expected<void, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  auto& memory = *loaded.value();
  auto changed = memory;
  const bool differs = offered ? changed.products.erase(product) > 0
                               : changed.products.insert(product).second;
  if (!differs) return {};
  if (const auto saved = save(account, changed); !saved) {
    return unexpected(saved.error());
  }
  changed.revision = ++changes_;
  memory = std::move(changed);
  return {};
}

auto TrialMemory::used(AccountId account) -> Expected<Used, Unanswered> {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  if (!loaded) return unexpected(loaded.error());
  const auto& memory = *loaded.value();
  return Used{{memory.products.begin(), memory.products.end()},
              memory.revision};
}

auto TrialMemory::is_used(AccountId account, const ProductId& product) -> bool {
  const platform::Lock lock{mutex_};
  const auto loaded = held(account);
  return loaded && loaded.value()->products.contains(product);
}

auto TrialMemory::forget(AccountId account) -> Expected<void, Unanswered> {
  const platform::Lock lock{mutex_};
  accounts_.erase(account.value());
  ++changes_;
  return cache_.erase<cache::UsedTrialRecord>(account);
}

auto TrialMemory::held(AccountId account) -> Expected<Held*, Unanswered> {
  auto& memory = accounts_[account.value()];
  if (memory.loaded) return &memory;
  const auto record = cache_.read<cache::UsedTrialRecord>(account);
  if (!record) return unexpected(record.error());
  if (record.value())
    memory.products.insert(record.value()->products().begin(),
                           record.value()->products().end());
  memory.revision = ++changes_;
  memory.loaded = true;
  return &memory;
}

auto TrialMemory::save(AccountId account, const Held& held)
    -> Expected<void, Unanswered> {
  if (held.products.empty())
    return cache_.erase<cache::UsedTrialRecord>(account);
  return cache_.write(cache::UsedTrialRecord::of(
      account, {held.products.begin(), held.products.end()}));
}

}  // namespace libchiaking::catalog
