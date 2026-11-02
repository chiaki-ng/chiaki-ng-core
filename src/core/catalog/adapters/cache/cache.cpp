#include "src/core/catalog/adapters/cache/cache.hpp"

#include <optional>
#include <span>
#include <string>
#include <utility>

namespace libchiaking::catalog::cache {

Cache::Cache(platform::RecordStore& store,
             const platform::ClockProvider& wall_clock)
    : store_(store), wall_clock_(wall_clock) {
  erase_leftovers();
}

auto Cache::now() const noexcept -> Time {
  return std::chrono::floor<std::chrono::milliseconds>(
      Time{} + std::chrono::microseconds{
                   static_cast<std::int64_t>(wall_clock_.now_microseconds())});
}

auto Cache::user_record(AccountId account, std::string_view leaf)
    -> std::optional<platform::RecordName> {
  const auto users = platform::RecordName::parse(users_segment);
  const auto user = users ? users->child(account.decimal()) : std::nullopt;
  return user ? user->child(leaf) : std::nullopt;
}

auto Cache::shared_record(std::string_view kind, std::string_view scope)
    -> std::optional<platform::RecordName> {
  const auto shelf = platform::RecordName::parse(kind);
  return shelf ? shelf->child(scope) : std::nullopt;
}

auto Cache::forget(AccountId account) -> Expected<void, Unanswered> {
  const auto users = platform::RecordName::parse(users_segment);
  const auto user = users ? users->child(account.decimal()) : std::nullopt;
  if (!user || !store_.erase_prefix(*user))
    return unexpected(Unanswered{Unanswered::Reason::storage});
  return {};
}

auto Cache::listed(std::string_view shelf)
    -> Expected<std::vector<platform::RecordListing>, Unanswered> {
  const auto prefix = platform::RecordName::parse(shelf);
  if (!prefix) return unexpected(Unanswered{Unanswered::Reason::storage});
  auto held = store_.list(*prefix);
  if (!held) return unexpected(Unanswered{Unanswered::Reason::storage});
  return std::move(held.value());
}

auto Cache::kept(const platform::RecordName& name)
    -> Expected<std::optional<Kept<Summary>>, Unanswered> {
  return kept_as<Summary>(name);
}

auto Cache::erase_shelf(std::string_view shelf) -> Expected<void, Unanswered> {
  const auto prefix = platform::RecordName::parse(shelf);
  if (!prefix || !store_.erase_prefix(*prefix))
    return unexpected(Unanswered{Unanswered::Reason::storage});
  return {};
}

auto Cache::erase_for_every_account(std::string_view leaf)
    -> Expected<void, Unanswered> {
  const auto kept = listed(users_segment);
  if (!kept) return unexpected(kept.error());
  const auto ending =
      std::string{platform::RecordName::separator} + std::string{leaf};
  std::optional<Unanswered> failure;
  for (const auto& record : kept.value())
    if (record.name.text().ends_with(ending))
      if (const auto erased = erase_named(record.name); !erased)
        failure = erased.error();
  if (failure) return unexpected(*failure);
  return {};
}

auto Cache::erase_named(const platform::RecordName& name)
    -> Expected<void, Unanswered> {
  const auto erased = store_.erase(name);
  if (!erased && erased.error() != platform::StorageError::not_found)
    return unexpected(Unanswered{Unanswered::Reason::storage});
  return {};
}

auto Cache::read_blob(const platform::RecordName& name)
    -> Expected<std::optional<std::vector<std::byte>>, Unanswered> {
  auto stored = store_.read(name);
  if (stored) return std::optional{std::move(stored.value().data)};
  if (stored.error() == platform::StorageError::not_found)
    return std::optional<std::vector<std::byte>>{};
  return unexpected(Unanswered{Unanswered::Reason::storage});
}

auto Cache::write_blob(const platform::RecordName& name,
                       std::span<const std::byte> bytes)
    -> Expected<void, Unanswered> {
  if (!store_.write_atomic(name, bytes))
    return unexpected(Unanswered{Unanswered::Reason::storage});
  return {};
}

auto Cache::owner_of(AccountId account) -> Owner {
  return Owner{.account = account.decimal()};
}

auto Cache::owner_of(const StoreLocale& locale) -> Owner {
  return Owner{.region = locale.text()};
}

auto Cache::owner_of(const Everyone&) -> Owner { return Owner{}; }

void Cache::discard(const platform::RecordName& name) {
  (void)store_.erase(name);
}

void Cache::erase_leftovers() {
  for (const auto file_name : old_file_names)
    (void)store_.erase_legacy_file(file_name);
  for (const auto record : old_record_names)
    if (const auto name = platform::RecordName::parse(record))
      (void)store_.erase(*name);
}

}  // namespace libchiaking::catalog::cache
