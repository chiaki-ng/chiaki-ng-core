#include "src/core/catalog/adapters/cache/held_answers.hpp"

#include "src/core/catalog/adapters/cache/game_details_record.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/played_games_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

HeldAnswers::HeldAnswers(platform::RecordStore& location,
                         platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock,
                         const platform::ClockProvider& wall_clock)
    : threading_(threading),
      clock_(clock),
      cache_(location, wall_clock),
      refusals_(cache_, threading),
      trials_(cache_, threading),
      images_(cache_, threading),
      disk_(threading),
      mutex_(threading),
      flights_(threading, clock) {}

auto HeldAnswers::now() const noexcept -> Time { return cache_.now(); }

auto HeldAnswers::is_servable(const StreamableList& list, Time now) -> bool {
  return list.is_complete() && list.origin() != ListOrigin::public_store &&
         now >= list.fetched_at() &&
         now - list.fetched_at() <= cache::StreamableListRecord::lifetime;
}

auto HeldAnswers::held_list(const StoreLocale& region) -> ListPtr {
  {
    const platform::Lock lock{mutex_};
    if (const auto found = lists_.find(region.lowercase());
        found != lists_.end())
      return found->second;
  }
  return fresh_list(region);
}

auto HeldAnswers::fresh_list(const StoreLocale& region) -> ListPtr {
  const auto key = region.lowercase();
  {
    const platform::Lock lock{mutex_};
    const auto found = lists_.find(key);
    if (found != lists_.end() && is_servable(*found->second, cache_.now()))
      return found->second;
  }
  const platform::Lock disk{disk_};
  auto stored = cache_.read<cache::StreamableListRecord>(region);
  if (!stored || !stored.value() ||
      !is_servable(stored.value()->list(), cache_.now()))
    return nullptr;
  auto list = std::make_shared<const StreamableList>(stored.value()->list());
  const platform::Lock lock{mutex_};
  lists_.insert_or_assign(key, list);
  return list;
}

void HeldAnswers::keep(const StoreLocale& region, const ListPtr& list) {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    lists_.insert_or_assign(region.lowercase(), list);
  }
  if (const auto record = cache::StreamableListRecord::of(*list))
    (void)cache_.write(*record);
}

auto HeldAnswers::fresh_library(AccountId account) -> LibraryPtr {
  {
    const platform::Lock lock{mutex_};
    if (auto held = ownership_.fresh(account, cache_.now())) return held;
  }
  const platform::Lock disk{disk_};
  auto stored = cache_.read<cache::LibraryRecord>(account);
  if (!stored || !stored.value() ||
      stored.value()->library().account() != account ||
      !cache::HeldOwnership::is_fresh(stored.value()->library(), cache_.now()))
    return nullptr;
  auto held = std::make_shared<const Library>(stored.value()->library());
  const auto& ps5 = stored.value()->ps5_games();
  const platform::Lock lock{mutex_};
  ownership_.hold_stored(
      account, held,
      ps5 ? std::make_shared<const OwnedPs5Games>(*ps5) : nullptr);
  return held;
}

auto HeldAnswers::kept(LibraryPtr owned, Ps5Ptr ps5) -> LibraryPtr {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    ownership_.hold_read(owned, ps5);
  }
  (void)cache_.write(cache::LibraryRecord{
      *owned, ps5 ? std::optional<OwnedPs5Games>{*ps5} : std::nullopt});
  return owned;
}

auto HeldAnswers::kept_with(AccountId account, OwnedTitle title) -> Owned {
  const platform::Lock disk{disk_};
  Owned rechecked;
  {
    const platform::Lock lock{mutex_};
    rechecked = ownership_.with(account, std::move(title), cache_.now());
  }
  if (rechecked.state == LibraryState::fetched)
    (void)cache_.write(cache::LibraryRecord{
        *rechecked.titles, rechecked.ps5
                               ? std::optional<OwnedPs5Games>{*rechecked.ps5}
                               : std::nullopt});
  return rechecked;
}

auto HeldAnswers::kept_with(AccountId account, Ps5Entitlement record) -> Owned {
  const auto now = cache_.now();
  const platform::Lock disk{disk_};
  Owned rechecked;
  {
    const platform::Lock lock{mutex_};
    rechecked = ownership_.with(account, std::move(record), now);
  }
  if (rechecked.state == LibraryState::fetched)
    (void)cache_.write(cache::LibraryRecord{*rechecked.titles, *rechecked.ps5});
  return rechecked;
}

auto HeldAnswers::owned_after(AccountId account, LibraryPtr fetched) -> Owned {
  const platform::Lock lock{mutex_};
  return ownership_.after_read(account, std::move(fetched));
}

void HeldAnswers::keep_ps5_alone(AccountId account, Ps5Ptr ps5) {
  const platform::Lock lock{mutex_};
  ownership_.hold_ps5_alone(account, std::move(ps5));
}

auto HeldAnswers::held_by(AccountId account) -> std::optional<Owned> {
  auto whole = fresh_library(account);
  const platform::Lock lock{mutex_};
  if (whole)
    return Owned{std::move(whole), LibraryState::fetched,
                 ownership_.ps5_of(account)};
  return ownership_.rechecked_alone(account);
}

void HeldAnswers::forget_library(AccountId account) {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    ownership_.drop_library(account);
  }
  (void)cache_.erase<cache::LibraryRecord>(account);
}

auto HeldAnswers::held_included(const StoreLocale& locale, int age)
    -> Included {
  return held_on(included_, Question::included_in(locale, age), locale, age);
}

auto HeldAnswers::keep(const StoreLocale& locale, int age, Included learned)
    -> CategoryPtr {
  return kept_on(included_, Question::included_in(locale, age),
                 std::move(learned));
}

auto HeldAnswers::held_for_sale(const StoreLocale& locale, int age) -> ForSale {
  return held_on(for_sale_, Question::for_sale_in(locale, age), locale, age);
}

auto HeldAnswers::keep(const StoreLocale& locale, int age, ForSale learned)
    -> ForSalePtr {
  return kept_on(for_sale_, Question::for_sale_in(locale, age),
                 std::move(learned));
}

template <class Category>
auto HeldAnswers::held_on(std::map<std::string, HeldCategory<Category>>& shelf,
                          const Question& question, const StoreLocale& locale,
                          int age) -> HeldCategory<Category> {
  using Record = cache::CategoryRecord<Category>;
  const auto key = std::string{question.text()};
  {
    const platform::Lock lock{mutex_};
    if (const auto found = shelf.find(key); found != shelf.end())
      return found->second;
  }
  const platform::Lock disk{disk_};
  auto stored = cache_.read<Record>(locale);
  auto held = held_from(stored && stored.value() && stored.value()->age() == age
                            ? std::move(*stored.value())
                            : Record::nothing_held(locale).for_age(age));
  const platform::Lock lock{mutex_};
  return shelf.try_emplace(key, std::move(held)).first->second;
}

template <class Category>
auto HeldAnswers::kept_on(std::map<std::string, HeldCategory<Category>>& shelf,
                          const Question& question,
                          HeldCategory<Category> learned)
    -> std::shared_ptr<const Category> {
  const platform::Lock disk{disk_};
  (void)cache_.write(*learned.record);
  const platform::Lock lock{mutex_};
  shelf.insert_or_assign(std::string{question.text()}, learned);
  return learned.category;
}

auto HeldAnswers::held_exclusions()
    -> std::optional<cloud_stream::ExclusionList> {
  {
    const platform::Lock lock{mutex_};
    if (exclusions_) return exclusions_;
  }
  const platform::Lock disk{disk_};
  const auto stored = cache_.read<cache::ExclusionListRecord>({});
  if (!stored || !stored.value()) return std::nullopt;
  const platform::Lock lock{mutex_};
  if (!exclusions_) exclusions_ = stored.value()->list();
  return exclusions_;
}

void HeldAnswers::keep(const cloud_stream::ExclusionList& exclusions) {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    exclusions_ = exclusions;
  }
  (void)cache_.write(cache::ExclusionListRecord{exclusions});
}

auto HeldAnswers::matched_before(AccountId account, const MatchedFrom& sources,
                                 const MyGames::Inputs& inputs) -> GamesPtr {
  const platform::Lock lock{mutex_};
  return builds_.matched_before(account, sources, inputs);
}

void HeldAnswers::keep_matched(AccountId account, const GamesPtr& games,
                               MatchedFrom sources) {
  const platform::Lock lock{mutex_};
  builds_.keep(account, games, std::move(sources));
}

auto HeldAnswers::fresh_storefront_answer(AccountId account,
                                          const ProductId& product)
    -> std::optional<StorefrontAnswer> {
  const platform::Lock lock{mutex_};
  return storefront_.fresh(account, product, cache_.now());
}

void HeldAnswers::keep(AccountId account, const ProductId& product,
                       const StorefrontAnswer& answer) {
  const platform::Lock lock{mutex_};
  storefront_.keep(account, product, answer);
}

void HeldAnswers::drop_storefront_answer(AccountId account,
                                         const ProductId& product) {
  const platform::Lock lock{mutex_};
  storefront_.drop(account, product);
}

auto HeldAnswers::fresh_access(const users::PsUser& user)
    -> std::optional<CloudAccess> {
  const auto account = user.account_id();
  const auto state = user.sign_in_state();
  const platform::Lock lock{mutex_};
  return access_.fresh(account, state, cache_.now());
}

void HeldAnswers::note_access(const users::PsUser& user,
                              const CloudAccess& access) {
  const auto state = user.sign_in_state();
  const platform::Lock lock{mutex_};
  access_.note(user.account_id(), state, access);
}

auto HeldAnswers::fresh_details(const StoreLocale& locale,
                                const std::optional<ProductId>& product,
                                const std::optional<ConceptId>& grouped_under)
    -> std::optional<GameDetails> {
  const platform::Lock disk{disk_};
  const auto stored =
      cache_.read<cache::GameDetailsRecord>({locale, product, grouped_under});
  if (!stored || !stored.value() ||
      !stored.value()->details().is_fresh_at(cache_.now()))
    return std::nullopt;
  return stored.value()->details();
}

void HeldAnswers::keep(const GameDetails& details) {
  const platform::Lock disk{disk_};
  const auto before = cache_.read<cache::GameDetailsRecord>(
      cache::GameDetailsRecord{details}.key());
  (void)cache_.write(cache::GameDetailsRecord{details});
  if (!before || !before.value()) return;
  const auto still_shown = details.images();
  std::vector<ImageRef> gone;
  for (const auto& image : before.value()->details().images())
    if (std::ranges::find(still_shown, image) == still_shown.end())
      gone.push_back(image);
  (void)images_.drop(gone);
}

auto HeldAnswers::forget_lists() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    lists_.clear();
  }
  return cache_.erase_shelf(cache::StreamableListRecord::kind);
}

auto HeldAnswers::forget_included() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    included_.clear();
  }
  return cache_.erase_shelf(cache::PremiumCategoryRecord::kind);
}

auto HeldAnswers::forget_for_sale() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    for_sale_.clear();
  }
  return cache_.erase_shelf(cache::ForSaleCategoryRecord::kind);
}

auto HeldAnswers::fresh_played(AccountId account) -> PlayedPtr {
  {
    const platform::Lock lock{mutex_};
    if (auto held = plays_.fresh(account, cache_.now())) return held;
  }
  const platform::Lock disk{disk_};
  auto stored = cache_.read<cache::PlayedGamesRecord>(account);
  if (!stored || !stored.value() ||
      !cache::HeldPlays::is_fresh(stored.value()->fetched_at(), cache_.now()))
    return nullptr;
  auto held =
      std::make_shared<const std::vector<PlayedGame>>(stored.value()->games());
  const platform::Lock lock{mutex_};
  plays_.hold(account, held, stored.value()->fetched_at());
  return held;
}

auto HeldAnswers::keep_played(AccountId account, std::vector<PlayedGame> games)
    -> PlayedPtr {
  const auto read_at = cache_.now();
  auto held = std::make_shared<const std::vector<PlayedGame>>(std::move(games));
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    plays_.hold(account, held, read_at);
  }
  (void)cache_.write(cache::PlayedGamesRecord{account, read_at, *held});
  return held;
}

auto HeldAnswers::forget_played() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    plays_.drop_everyone();
  }
  return cache_.erase_for_every_account(cache::PlayedGamesRecord::kind);
}

auto HeldAnswers::forget_libraries() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    ownership_.drop_every_library();
  }
  return cache_.erase_for_every_account(cache::LibraryRecord::kind);
}

auto HeldAnswers::forget_refusals() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  return refusals_.forget_everyone();
}

auto HeldAnswers::forget_details() -> Expected<void, Unanswered> {
  const platform::Lock disk{disk_};
  const auto kept = cache_.listed(cache::GameDetailsRecord::kind);
  if (!kept) return unexpected(kept.error());
  std::vector<ImageRef> shown;
  for (const auto& record : kept.value()) {
    const auto held =
        cache_.kept_as<cache::GameDetailsRecord::Body>(record.name);
    if (!held || !held.value()) continue;
    const auto& body = held.value()->body;
    const auto locale = StoreLocale::parse(held.value()->region.value_or(""));
    const auto key =
        locale ? cache::GameDetailsRecord::key_of(*locale, body) : std::nullopt;
    const auto details =
        key ? cache::GameDetailsRecord::from_body(body, *key) : std::nullopt;
    if (details)
      std::ranges::move(details->details().images(), std::back_inserter(shown));
  }
  const auto dropped = images_.drop(shown);
  const auto erased = cache_.erase_shelf(cache::GameDetailsRecord::kind);
  if (!erased) return erased;
  return dropped;
}

auto HeldAnswers::forget_images() -> Expected<void, Unanswered> {
  return images_.clear();
}

auto HeldAnswers::starting_over(AccountId account) -> bool {
  const platform::Lock lock{mutex_};
  return starting_over_.contains(account.value());
}

void HeldAnswers::started_afresh(AccountId account) {
  const platform::Lock lock{mutex_};
  starting_over_.erase(account.value());
}

void HeldAnswers::start_over(AccountId account,
                             const std::optional<StoreLocale>& region) {
  fetch_board(account).start_over();
  drop_answers(account, region);
}

void HeldAnswers::drop_answers(AccountId account,
                               const std::optional<StoreLocale>& region) {
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    starting_over_.insert(account.value());
    ownership_.forget(account);
    builds_.forget(account);
    storefront_.forget(account);
    access_.forget(account);
    plays_.forget(account);
    if (region) lists_.erase(region->lowercase());
  }
  (void)cache_.erase<cache::LibraryRecord>(account);
  (void)cache_.erase<cache::PlayedGamesRecord>(account);
  if (region) (void)cache_.erase<cache::StreamableListRecord>(*region);
}

void HeldAnswers::forget(AccountId account) {
  fetch_board(account).start_over();
  const platform::Lock disk{disk_};
  {
    const platform::Lock lock{mutex_};
    starting_over_.erase(account.value());
    ownership_.forget(account);
    builds_.forget(account);
    storefront_.forget(account);
    access_.forget(account);
    plays_.forget(account);
  }
  (void)cache_.forget(account);
  (void)refusals_.forget(account);
  (void)trials_.forget(account);
}

auto HeldAnswers::fetch_board(AccountId account) -> FetchBoard& {
  const platform::Lock lock{mutex_};
  auto& board = boards_[account.value()];
  if (!board) board = std::make_unique<FetchBoard>(threading_, clock_);
  return *board;
}

}  // namespace libchiaking::catalog
