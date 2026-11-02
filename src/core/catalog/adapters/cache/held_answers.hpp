/// @file
/// @brief Declares HeldAnswers, everything the catalog holds between requests:
/// the answers in memory, their records on disk, the refusal memory, the image
/// shelf, and the shared flights that let concurrent callers share one request.
///
/// The use cases ask HeldAnswers first and go to PS only when nothing usable
/// is held. It decides what "usable" means for each answer (complete,
/// from the account's service, within its lifetime, for the same account and
/// sign-in) and keeps memory and disk in step.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/detail/catalog/played_game.hpp"
#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/exclusion_list_record.hpp"
#include "src/core/catalog/adapters/cache/flight_question.hpp"
#include "src/core/catalog/adapters/cache/held_access.hpp"
#include "src/core/catalog/adapters/cache/held_builds.hpp"
#include "src/core/catalog/adapters/cache/held_ownership.hpp"
#include "src/core/catalog/adapters/cache/held_plays.hpp"
#include "src/core/catalog/adapters/cache/held_storefront.hpp"
#include "src/core/catalog/adapters/cache/image_shelf.hpp"
#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/adapters/cache/refusal_memory.hpp"
#include "src/core/catalog/adapters/cache/trial_memory.hpp"
#include "src/core/catalog/adapters/fetch_board.hpp"
#include "src/core/catalog/model/cloud_stream/exclusions.hpp"
#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/users/cloud_streaming.hpp"
#include "src/core/utils/concurrency/shared_flight.hpp"

#include <libchiaking/platform/storage/record_store.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::catalog {

/// @brief The catalog's held answers for every account and region.
///
/// Two locks guard it: a disk lock that serialises every record read and write
/// (and is always taken before the memory lock), and a memory lock over the
/// in-memory maps and the per-account parts, held only briefly. The parts are
/// called only under the memory lock, so start_over, drop_answers and forget
/// clear every part for an account together. Every member is safe to call from
/// any thread; members that touch the disk block on its I/O, and answer blocks
/// for as long as the fetch it shares. Answers are handed out as shared
/// pointers to const, so a caller keeps a consistent view after the held answer
/// is replaced. The record store and clock must outlive it. Not copyable.
class HeldAnswers {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  /// @brief A held streamable list.
  using ListPtr = std::shared_ptr<const StreamableList>;
  /// @brief A held library.
  using LibraryPtr = std::shared_ptr<const Library>;
  /// @brief A held MyGames build.
  using GamesPtr = std::shared_ptr<const MyGames>;
  /// @brief A held Premium category.
  using CategoryPtr = std::shared_ptr<const cloud_stream::PremiumCategory>;
  /// @brief A held public Store streaming category.
  using ForSalePtr = std::shared_ptr<const cloud_stream::ForSaleCategory>;
  /// @brief A held PS5 entitlement scan.
  using Ps5Ptr = std::shared_ptr<const OwnedPs5Games>;
  /// @brief A held play history.
  using PlayedPtr = std::shared_ptr<const std::vector<PlayedGame>>;

  /// @brief What an account owns, as far as is held.
  using Owned = cache::HeldOwnership::Owned;

  /// @brief A category record and the category it holds, shared.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  template <class Category>
  struct HeldCategory {
    /// @brief The record, with its failure and age, shared by every holder.
    std::shared_ptr<const cache::CategoryRecord<Category>> record;
    /// @brief The record's category inside the shared record; null when none
    ///     has been fetched.
    std::shared_ptr<const Category> category{};
  };
  /// @brief The held Premium category.
  using Included = HeldCategory<cloud_stream::PremiumCategory>;
  /// @brief The held for-sale category.
  using ForSale = HeldCategory<cloud_stream::ForSaleCategory>;

  /// @brief The answers a MyGames build was made from, compared by identity to
  /// tell whether the held build is still current.
  using MatchedFrom = cache::HeldBuilds::MatchedFrom;

  /// @brief A MyGames build with the list and category it was built from.
  struct View {
    /// @brief The games; never null.
    GamesPtr games;
    /// @brief The LegacyCloudStream list, or why it could not be had.
    Expected<ListPtr, Unanswered> list;
    /// @brief The Premium category (null when the audience is unknown), or why
    /// it could not be had.
    Expected<CategoryPtr, Unanswered> category;
  };

  /// @brief A whole fetch's answer, with the start-over epoch the attempt
  /// that made it began under.
  struct Fetched {
    /// @brief The epoch, as FetchBoard counts start-overs.
    std::uint64_t epoch{};
    /// @brief The attempt that made it; 0 when the board did not start one.
    std::uint64_t attempt{};
    /// @brief The view, or why the fetch ended without one; never a
    /// cancellation.
    Expected<View, Unanswered> view;
  };

  /// @brief One shared flight per kind of question, so concurrent callers
  /// asking the same question share one request.
  struct Flights {
    /// @brief Makes every flight with nothing in the air.
    ///
    /// @param threading Makes each flight's lock and condition variable.
    /// @param clock The monotonic clock the flights' waits measure on.
    Flights(platform::ThreadingProvider& threading,
            const platform::ClockProvider& clock)
        : fetches(threading, clock),
          lists(threading, clock),
          libraries(threading, clock),
          included(threading, clock),
          for_sale(threading, clock),
          storefront(threading, clock),
          rechecks(threading, clock),
          access(threading, clock),
          details(threading, clock),
          images(threading, clock),
          exclusions(threading, clock),
          played(threading, clock) {}

    /// @brief Whole fetches of an account's games, by account.
    concurrency::SharedFlight<Fetched, Unanswered> fetches;
    /// @brief Streamable list walks, by region.
    concurrency::SharedFlight<ListPtr, Unanswered> lists;
    /// @brief Library reads, by account.
    concurrency::SharedFlight<LibraryPtr, Unanswered> libraries;
    /// @brief Premium category scans, by locale and age.
    concurrency::SharedFlight<CategoryPtr, Unanswered> included;
    /// @brief For-sale category scans, by locale and age.
    concurrency::SharedFlight<ForSalePtr, Unanswered> for_sale;
    /// @brief Storefront answers, by account and product.
    concurrency::SharedFlight<StorefrontAnswer, Unanswered> storefront;
    /// @brief Ownership rechecks, by account and product.
    concurrency::SharedFlight<OwnershipAnswer, Unanswered> rechecks;
    /// @brief Cloud access reads, by account.
    concurrency::SharedFlight<CloudAccess, Unanswered> access;
    /// @brief Game details fetches, by locale and product or concept.
    concurrency::SharedFlight<GameDetails, Unanswered> details;
    /// @brief Image downloads, by URL.
    concurrency::SharedFlight<cache::ImageShelf::Bytes, Unanswered> images;
    /// @brief Exclusion list refreshes.
    concurrency::SharedFlight<cloud_stream::ExclusionList, Unanswered>
        exclusions;
    /// @brief Play history reads, by account.
    concurrency::SharedFlight<PlayedPtr, Unanswered> played;
  };

  /// @brief The text that names a question for a shared flight.
  using Question = cache::FlightQuestion;

  /// @brief Opens the held answers over a record store.
  ///
  /// Erases the files of earlier versions; nothing else is read until asked.
  ///
  /// @param location Where records are kept. Must outlive this object.
  /// @param threading Makes the locks and the waits of shared flights. Must
  ///     outlive this object.
  /// @param clock The monotonic clock those waits measure on. Must outlive
  ///     this object.
  /// @param wall_clock Stamps and ages answers. Must outlive this object.
  HeldAnswers(platform::RecordStore& location,
              platform::ThreadingProvider& threading,
              const platform::ClockProvider& clock,
              const platform::ClockProvider& wall_clock);
  /// @brief Not copyable: it owns locks, memory and flights.
  HeldAnswers(const HeldAnswers&) = delete;
  /// @brief Not copyable: it owns locks, memory and flights.
  auto operator=(const HeldAnswers&) -> HeldAnswers& = delete;

  /// @brief Answers a question from what is held, or fetches it once for every
  /// concurrent asker.
  ///
  /// Checks held first; when nothing is held, joins or starts the flight for
  /// the question, checks held again inside it, and only then fetches. Blocks
  /// while another caller's fetch for the same question runs, and while the
  /// fetch runs.
  ///
  /// @tparam Answer The answer type.
  /// @tparam Held A callable returning the held answer as an std::optional or
  ///     an Expected; empty or an error means nothing usable is held.
  /// @tparam Fetch A callable returning Expected<Answer, Unanswered>.
  /// @param flight The flight for this kind of question.
  /// @param question The question.
  /// @param stop Abandons the wait (or the fetch, when the fetch reads it).
  /// @param held Reads the held answer.
  /// @param fetch Asks PS.
  /// @return The held or fetched answer; the fetch's failure; or a cancellation
  ///     when stop fires while waiting.
  template <class Answer, class Held, class Fetch>
  [[nodiscard]] static auto answer(
      concurrency::SharedFlight<Answer, Unanswered>& flight,
      const Question& question, const platform::StopToken& stop,
      const Held& held, const Fetch& fetch) -> Expected<Answer, Unanswered> {
    const auto kept = [&held]() -> std::optional<Answer> {
      auto found = held();
      if (!found) return std::nullopt;
      if constexpr (requires { found.value(); })
        return std::move(found).value();
      else
        return found;
    };
    if (auto found = kept()) return std::move(*found);
    return flight.answer(question.text(), stop,
                         [&]() -> Expected<Answer, Unanswered> {
                           if (auto found = kept()) return std::move(*found);
                           return fetch();
                         });
  }
  /// @brief The wall clock now.
  ///
  /// @return Wall-clock time floored to milliseconds.
  auto now() const noexcept -> Time;
  /// @brief The shared flights.
  ///
  /// @return The flights, valid while this object lives.
  auto in_flight() noexcept -> Flights& { return flights_; }
  /// @brief The board that follows an account's whole fetch.
  ///
  /// Made on first use and kept while this object lives, so the reference
  /// stays valid.
  ///
  /// @param account The account.
  /// @return The account's board.
  auto fetch_board(AccountId account) -> FetchBoard&;
  /// @brief The refusal memory.
  ///
  /// @return The memory, valid while this object lives.
  auto refusals() noexcept -> RefusalMemory& { return refusals_; }
  /// @brief The memory of store games whose trial each account has used.
  ///
  /// @return The memory, valid while this object lives.
  auto trials() noexcept -> TrialMemory& { return trials_; }
  /// @brief The typed record store.
  ///
  /// @return The cache, valid while this object lives. Callers outside this
  ///     class do not take the disk lock.
  auto records() noexcept -> cache::Cache& { return cache_; }
  /// @brief The image shelf.
  ///
  /// @return The shelf, valid while this object lives.
  auto images() noexcept -> cache::ImageShelf& { return images_; }

  /// @brief Any list held in memory for a region, else a usable one from disk.
  ///
  /// The in-memory list is returned whatever its age; use fresh_list to insist
  /// on a usable one.
  ///
  /// @param region The region.
  /// @return The list, or nullptr when none is held.
  auto held_list(const StoreLocale& region) -> ListPtr;
  /// @brief A usable list for a region, from memory or disk.
  ///
  /// Usable means complete, from the account's service and built no more than
  /// 24 hours ago. A usable list read from disk replaces the one in memory.
  ///
  /// @param region The region.
  /// @return The list, or nullptr when no usable list is held.
  auto fresh_list(const StoreLocale& region) -> ListPtr;
  /// @brief Holds a list, writing it to disk when it is complete and from the
  /// account's service; write failures are ignored.
  ///
  /// @param region The region.
  /// @param list The list; must not be null.
  void keep(const StoreLocale& region, const ListPtr& list);

  /// @brief A library fetched no more than 24 hours ago, from memory or disk.
  ///
  /// A library read from disk is held in memory with its PS5 scan.
  ///
  /// @param account The account.
  /// @return The library, or nullptr when none is held within its lifetime.
  auto fresh_library(AccountId account) -> LibraryPtr;
  /// @brief Holds a freshly read library and writes it to disk.
  ///
  /// Clears anything learned while the library was unavailable.
  ///
  /// @param owned The library; must not be null.
  /// @param ps5 Its PS5 scan; may be null.
  /// @return The library.
  auto kept(LibraryPtr owned, Ps5Ptr ps5) -> LibraryPtr;
  /// @brief Adds or replaces one LegacyCloudStream title after a recheck.
  ///
  /// When no library is held the title goes into a library of rechecked titles
  /// only, marked unavailable, and nothing is written to disk.
  ///
  /// @param account The account.
  /// @param title The rechecked title.
  /// @return What the account is now known to own.
  auto kept_with(AccountId account, OwnedTitle title) -> Owned;
  /// @brief Adds or replaces one PS5 entitlement after a recheck.
  ///
  /// The record replaces its entry in the PS5 scan, and its title (when it is a
  /// game) the matching library title. Without a held library both go into the
  /// rechecked-only state and nothing is written.
  ///
  /// @param account The account.
  /// @param record The rechecked entitlement.
  /// @return What the account is now known to own.
  auto kept_with(AccountId account, Ps5Entitlement record) -> Owned;
  /// @brief What the account owns after a library read.
  ///
  /// @param account The account.
  /// @param fetched The library read, or nullptr when it could not be read.
  /// @return The fetched library as LibraryState::fetched; or, when it could
  /// not
  ///     be read, the titles rechecked so far (possibly none) as
  ///     LibraryState::unavailable.
  auto owned_after(AccountId account, LibraryPtr fetched) -> Owned;
  /// @brief Holds a PS5 scan read while the commerce library could not be.
  ///
  /// Its games become the PS5 part of the rechecked-only library, marked
  /// unavailable; nothing is written.
  ///
  /// @param account The account.
  /// @param ps5 The scan; must not be null.
  void keep_ps5_alone(AccountId account, Ps5Ptr ps5);
  /// @brief What the account is known to own, without asking PS.
  ///
  /// @param account The account.
  /// @return A library within its lifetime as fetched; else the rechecked-only
  ///     library as unavailable after a failed read; else std::nullopt.
  auto held_by(AccountId account) -> std::optional<Owned>;
  /// @brief Drops the account's library and PS5 scan from memory and disk.
  ///
  /// @param account The account.
  void forget_library(AccountId account);

  /// @brief The held Premium category for a locale and age.
  ///
  /// Read from disk on first use; a stored record for another age counts as
  /// nothing held.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @return The record and category; the category may be null.
  auto held_included(const StoreLocale& locale, int age) -> Included;
  /// @brief Holds a Premium category record and writes it.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @param learned The new record.
  /// @return Its category; may be null.
  auto keep(const StoreLocale& locale, int age, Included learned)
      -> CategoryPtr;
  /// @brief The held for-sale category for a locale and age.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @return The record and category; the category may be null.
  auto held_for_sale(const StoreLocale& locale, int age) -> ForSale;
  /// @brief Holds a for-sale category record and writes it.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @param learned The new record.
  /// @return Its category; may be null.
  auto keep(const StoreLocale& locale, int age, ForSale learned) -> ForSalePtr;
  /// @brief Shares a record and points at the category it holds.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  /// @param record The record.
  /// @return The shared record with its category, or a null category when it
  ///     holds none.
  template <class Category>
  static auto held_from(cache::CategoryRecord<Category> record)
      -> HeldCategory<Category> {
    auto shared = std::make_shared<const cache::CategoryRecord<Category>>(
        std::move(record));
    const auto& category = shared->category();
    auto inside = category ? std::shared_ptr<const Category>{shared, &*category}
                           : nullptr;
    return HeldCategory<Category>{std::move(shared), std::move(inside)};
  }

  /// @brief The last good exclusion snapshot, from memory or disk.
  ///
  /// @return The snapshot whatever its age, or std::nullopt when none was ever
  ///     kept.
  auto held_exclusions() -> std::optional<cloud_stream::ExclusionList>;
  /// @brief Holds an exclusion snapshot and writes it.
  ///
  /// @param exclusions The snapshot.
  void keep(const cloud_stream::ExclusionList& exclusions);

  /// @brief The held MyGames build, when it is still current.
  ///
  /// @param account The account.
  /// @param sources The answers a new build would use.
  /// @param inputs The inputs a new build would use.
  /// @return The held build when it was made from the same answers, with alike
  ///     streaming access, and from the same inputs; nullptr otherwise.
  auto matched_before(AccountId account, const MatchedFrom& sources,
                      const MyGames::Inputs& inputs) -> GamesPtr;
  /// @brief Holds a MyGames build and the answers it was made from.
  ///
  /// @param account The account.
  /// @param games The build.
  /// @param sources What it was made from.
  void keep_matched(AccountId account, const GamesPtr& games,
                    MatchedFrom sources);

  /// @brief A storefront answer that is still fresh.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @return The answer, or std::nullopt when none is held, it is for another
  ///     account, or it is no longer fresh.
  auto fresh_storefront_answer(AccountId account, const ProductId& product)
      -> std::optional<StorefrontAnswer>;
  /// @brief Holds a storefront answer in memory.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @param answer The answer.
  void keep(AccountId account, const ProductId& product,
            const StorefrontAnswer& answer);
  /// @brief Drops a held storefront answer, as after an ownership recheck.
  ///
  /// @param account The account.
  /// @param product The product.
  void drop_storefront_answer(AccountId account, const ProductId& product);

  /// @brief The cloud access last shown to a user, while still fresh and for
  /// the same sign-in.
  ///
  /// @param user The user.
  /// @return The access, or std::nullopt when none is held, the sign-in state
  ///     changed, it is for another account, or it is no longer fresh.
  auto fresh_access(const users::PsUser& user) -> std::optional<CloudAccess>;
  /// @brief Holds the cloud access shown to a user under the current sign-in.
  ///
  /// @param user The user.
  /// @param access The access.
  void note_access(const users::PsUser& user, const CloudAccess& access);

  /// @brief Fresh game details from disk.
  ///
  /// @param locale The locale.
  /// @param product The product, when asked by product.
  /// @param grouped_under The concept, when asked by concept.
  /// @return The details, or std::nullopt when none are stored or they are no
  ///     longer fresh.
  auto fresh_details(const StoreLocale& locale,
                     const std::optional<ProductId>& product,
                     const std::optional<ConceptId>& grouped_under)
      -> std::optional<GameDetails>;
  /// @brief Writes game details and drops the images the previous details
  /// showed that these no longer do.
  ///
  /// @param details The details.
  void keep(const GameDetails& details);

  /// @brief Forgets every streamable list in memory and on disk.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed.
  [[nodiscard]] auto forget_lists() -> Expected<void, Unanswered>;
  /// @brief Forgets every held Premium category in memory and on disk.
  ///
  /// The for-sale categories are not touched.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed.
  [[nodiscard]] auto forget_included() -> Expected<void, Unanswered>;
  /// @brief Forgets every held for-sale category in memory and on disk.
  ///
  /// The Premium categories are not touched. Takes the disk lock, then the
  /// memory lock; blocks while another record read or write runs.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed.
  [[nodiscard]] auto forget_for_sale() -> Expected<void, Unanswered>;
  /// @brief Forgets every account's library and PS5 scan in memory and on disk.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed for any record.
  [[nodiscard]] auto forget_libraries() -> Expected<void, Unanswered>;
  /// @brief Forgets every account's refusals in memory and on disk.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed for any record.
  [[nodiscard]] auto forget_refusals() -> Expected<void, Unanswered>;
  /// @brief Forgets every stored game details record and the images they show.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed.
  [[nodiscard]] auto forget_details() -> Expected<void, Unanswered>;
  /// @brief Forgets every kept image.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when the store
  ///     failed.
  [[nodiscard]] auto forget_images() -> Expected<void, Unanswered>;

  /// @brief Whether the user asked to start over and the next list has not run
  /// yet.
  ///
  /// @param account The account.
  /// @return True between start_over and started_afresh.
  auto starting_over(AccountId account) -> bool;
  /// @brief Notes that a list ran after start_over.
  ///
  /// @param account The account.
  void started_afresh(AccountId account);
  /// @brief A play history read no more than six hours ago, from memory or
  /// disk.
  ///
  /// @param account The account.
  /// @return The history, or nullptr when none is held within its lifetime.
  auto fresh_played(AccountId account) -> PlayedPtr;
  /// @brief Holds a freshly read play history, stamped now, and writes it to
  /// disk; write failures are ignored.
  ///
  /// @param account The account.
  /// @param games The games, in PSN's order.
  /// @return The held history.
  auto keep_played(AccountId account, std::vector<PlayedGame> games)
      -> PlayedPtr;
  /// @brief Drops every account's play history from memory and disk.
  ///
  /// @return Nothing on success; Unanswered::Reason::storage when a record
  ///     could not be listed or erased.
  [[nodiscard]] auto forget_played() -> Expected<void, Unanswered>;

  /// @brief Drops everything held for an account so the next list asks again.
  ///
  /// Clears the account's memory, its library and play history records and,
  /// when a region is
  /// given, that region's list in memory and on disk. Categories are refetched
  /// only when older than their refetch cooldown.
  ///
  /// A fetch running for the account is left to end; its answer is from
  /// before the start-over, and the next fetch drops what it kept with
  /// drop_answers before asking again.
  ///
  /// @param account The account.
  /// @param region The account's region, when known.
  void start_over(AccountId account, const std::optional<StoreLocale>& region);
  /// @brief Drops what start_over drops, without starting a new epoch.
  ///
  /// @param account The account.
  /// @param region The account's region, when known.
  void drop_answers(AccountId account,
                    const std::optional<StoreLocale>& region);
  /// @brief Forgets an account entirely: memory, refusals and every record
  /// under user/{account}.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief Whether a list may be served.
  ///
  /// @param list The list.
  /// @param now The wall-clock time now.
  /// @return True when complete, from the account's own store (its service or
  ///     its country's public store), and built no more
  ///     than 24 hours before now (and not after it).
  static auto is_servable(const StreamableList& list, Time now) -> bool;
  /// @brief The held category record for a question, reading disk on first use.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  /// @param shelf The in-memory map for this category kind.
  /// @param question The question naming the locale and age.
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @return The held record; nothing held when the stored one is for another
  ///     age or absent.
  template <class Category>
  auto held_on(std::map<std::string, HeldCategory<Category>>& shelf,
               const Question& question, const StoreLocale& locale, int age)
      -> HeldCategory<Category>;
  /// @brief Holds a category record and writes it; write failures are ignored.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  /// @param shelf The in-memory map for this category kind.
  /// @param question The question naming the locale and age.
  /// @param learned The new record.
  /// @return Its category; may be null.
  template <class Category>
  auto kept_on(std::map<std::string, HeldCategory<Category>>& shelf,
               const Question& question, HeldCategory<Category> learned)
      -> std::shared_ptr<const Category>;

  /// @brief Makes the fetch boards' locks.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock the fetch boards wait on.
  const platform::ClockProvider& clock_;
  /// @brief Typed access to the record store.
  cache::Cache cache_;
  /// @brief Remembered refusals.
  RefusalMemory refusals_;
  /// @brief Remembered used trials.
  TrialMemory trials_;
  /// @brief Kept images.
  cache::ImageShelf images_;

  /// @brief Serialises record reads and writes; taken before mutex_.
  platform::Mutex disk_;
  /// @brief Guards every in-memory map below.
  platform::Mutex mutex_;
  /// @brief Streamable lists by lower-case region.
  std::map<std::string, ListPtr> lists_;
  /// @brief Premium category records by question text.
  std::map<std::string, Included> included_;
  /// @brief For-sale category records by question text.
  std::map<std::string, ForSale> for_sale_;
  /// @brief The last good exclusion snapshot.
  std::optional<cloud_stream::ExclusionList> exclusions_;
  /// @brief Ids of the accounts between start_over and started_afresh.
  std::set<std::uint64_t> starting_over_;
  /// @brief What each account is known to own.
  cache::HeldOwnership ownership_;
  /// @brief Each account's latest MyGames build.
  cache::HeldBuilds builds_;
  /// @brief Each account's storefront answers.
  cache::HeldStorefront storefront_;
  /// @brief Each account's last shown cloud access.
  cache::HeldAccess access_;
  /// @brief Each account's play history.
  cache::HeldPlays plays_;
  /// @brief Per-account fetch boards by account id; never erased.
  std::map<std::uint64_t, std::unique_ptr<FetchBoard>> boards_;

  /// @brief The shared flights.
  Flights flights_;
};

}  // namespace libchiaking::catalog
