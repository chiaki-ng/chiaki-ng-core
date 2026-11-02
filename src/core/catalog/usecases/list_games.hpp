/// @file
/// @brief Declares usecases::ListGames, the use case that answers "which games
/// can this user stream": it gathers the LegacyCloudStream list, the library,
/// the PS5 categories and cloud access, and assembles MyGames.
///
/// Each answer comes from HeldAnswers when it is usable and from PS
/// otherwise, with concurrent callers sharing one request. Failures of one
/// source degrade the answer instead of failing it, except cancellation and a
/// signed-out or expired sign-in, which end the load.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/detail/catalog/fetch_report.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/usecases/asker.hpp"
#include "src/core/catalog/usecases/fetch_progress.hpp"
#include "src/core/catalog/usecases/read_categories.hpp"
#include "src/core/catalog/usecases/read_library.hpp"
#include "src/core/catalog/usecases/read_streamable_list.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <optional>

namespace libchiaking::catalog::usecases {

/// @brief Lists a user's games and the answers they are built from.
///
/// Borrows the HTTP provider, clocks, log and held answers, which must outlive
/// it. Safe to call from several threads at once: all shared state lives in
/// HeldAnswers. Calls taking a stop token block on PS for as long as the
/// answers they need take to fetch; the others read only what is held. Not
/// copyable.
class ListGames {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A held streamable list.
  using ListPtr = HeldAnswers::ListPtr;
  /// @brief A held library.
  using LibraryPtr = HeldAnswers::LibraryPtr;
  /// @brief A held MyGames build.
  using GamesPtr = HeldAnswers::GamesPtr;
  /// @brief A held Premium category.
  using CategoryPtr = HeldAnswers::CategoryPtr;
  /// @brief A held for-sale category.
  using ForSalePtr = HeldAnswers::ForSalePtr;
  /// @brief What a caller wants from a held category.
  using Demand = cache::category::Demand;

  /// @brief A MyGames build with the list and category it was built from.
  using View = HeldAnswers::View;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends every request. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param trusted_clock Refreshed when a shown game awaits its release; may
  /// be
  ///     null. Must outlive this object when given.
  /// @param log Receives log lines; may be null.
  /// @param held Where answers are held. Must outlive this object.
  ListGames(platform::HttpProvider& http, const platform::ClockProvider& clock,
            const platform::ClockProvider& wall_clock,
            TrustedClock* trusted_clock, platform::DiagnosticsProvider* log,
            HeldAnswers& held);
  /// @brief Not copyable: it borrows shared state.
  ListGames(const ListGames&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const ListGames&) -> ListGames& = delete;

  /// @brief The user's games.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param observer Told every state of the fetch, as view describes.
  /// @return The games; otherwise as view fails.
  [[nodiscard]] auto games(users::PsUser& user, const Stop& stop,
                           const FetchObserver& observer = {})
      -> Expected<GamesPtr, Unanswered>;
  /// @brief Builds the user's games, fetching whatever is not usably held.
  ///
  /// In order: the LegacyCloudStream list, the library (LegacyCloudStream and
  /// PS5), the Premium and for-sale categories (refetched after start_over
  /// unless fetched within the hour), and cloud streaming access. Then
  /// refreshes trusted time when a shown game awaits its release, and refreshes
  /// the exclusion list when a PS5 game is shown. Blocks for every request
  /// needed.
  ///
  /// One fetch runs per account: a caller arriving while one runs joins it
  /// and gets its view. A caller that arrives after a start-over and gets
  /// the view of an attempt begun before it leads a new attempt instead, so
  /// its answer is never older than its start-over. Each step is fed to the
  /// account's FetchBoard.
  ///
  /// @param user The user.
  /// @param stop Abandons the call; for a joining caller, only its wait.
  /// @param observer Told every state of the fetch this caller leads or
  ///     joins, on the thread running it, and never after this returns; may
  ///     be empty. A joining caller is told the current state first.
  /// @return The view; the sign-in failure when the user is signed out or
  ///     expired; a cancellation; the library's ending failure; or the list's
  ///     failure when there is no list, no Premium category and no PS5 scan.
  [[nodiscard]] auto view(users::PsUser& user, const Stop& stop,
                          const FetchObserver& observer = {})
      -> Expected<View, Unanswered>;
  /// @brief The latest state of the user's whole fetch.
  ///
  /// @param user The user.
  /// @return The state; FetchStep::idle before any fetch.
  auto fetch_state(const users::PsUser& user) -> FetchState;
  /// @brief Builds the user's games from held answers only; never asks PS.
  ///
  /// @param user The user.
  /// @return The view, or std::nullopt when no ownership is held, or neither a
  ///     list, a category nor a PS5 scan is.
  auto held_view(users::PsUser& user) -> std::optional<View>;
  /// @brief Records that PS refused to start a title, as
  /// Catalog::remember_refusal describes.
  ///
  /// The refusal is kept with the user's tier and how the held games hold
  /// the title now. A StreamRefusal::Kind::no_game_for_entitlement refusal
  /// also drops the held library. Nothing is recorded for a user turned
  /// away; a failure to write the record is not reported.
  ///
  /// @param user The user who was refused.
  /// @param product The title that was refused.
  /// @param kind What PS said.
  void remember_refusal(users::PsUser& user, const ProductId& product,
                        StreamRefusal::Kind kind);
  /// @brief Records whether the signed-in store still offers the user a
  /// store game's trial.
  ///
  /// A store game listed as offering a Play First Trial is remembered as
  /// used when the store no longer offers it, and forgotten when the store
  /// offers it again, so later builds label it accordingly. The record is
  /// kept under the product the held games list the title as, whatever
  /// alias was asked. Nothing is recorded for a user turned away or for
  /// any other game; a failure to write the record is not reported.
  ///
  /// @param user The user the store answered.
  /// @param product The product asked about.
  /// @param offered Whether the store offered the user a trial of it.
  void note_trial_offer(users::PsUser& user, const ProductId& product,
                        bool offered);
  /// @brief The user's streamable list, held or fetched.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return As ReadStreamableList::list.
  [[nodiscard]] auto streamable_list(users::PsUser& user, const Stop& stop)
      -> Expected<ListPtr, Unanswered>;
  /// @brief The user's library, held or fetched.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return As ReadLibrary::library.
  [[nodiscard]] auto library(users::PsUser& user, const Stop& stop)
      -> Expected<LibraryPtr, Unanswered>;
  /// @brief Rereads the user's whole library, ignoring what is held.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return As library.
  [[nodiscard]] auto library_afresh(users::PsUser& user, const Stop& stop)
      -> Expected<LibraryPtr, Unanswered>;
  /// @brief Whether the user may use LegacyCloudStream, from a fresh held
  /// answer or a walk of the shelves.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @return The access; the sign-in failure; a cancellation.
  [[nodiscard]] auto cloud_access(users::PsUser& user, const Stop& stop)
      -> Expected<CloudAccess, Unanswered>;
  /// @brief Assembles MyGames from given answers, reusing the held build when
  /// it was made from the same answers and inputs.
  ///
  /// Reads the standing refusals (judged against the user's tier, or by their
  /// own circumstances when it is unknown), and clears refusals whose holding
  /// changed. Does not ask PS.
  ///
  /// @param user The user.
  /// @param list The LegacyCloudStream list; may be null.
  /// @param owned What the user owns.
  /// @param category The Premium category; may be null.
  /// @param for_sale The for-sale category; may be null.
  /// @return The games; never null.
  auto matched(users::PsUser& user, const ListPtr& list,
               const HeldAnswers::Owned& owned, const CategoryPtr& category,
               const ForSalePtr& for_sale) -> GamesPtr;

  /// @brief The held Premium category for the user's audience.
  ///
  /// @param user The user.
  /// @return The category, or null when none is held or the audience is
  ///     unknown.
  auto category_held_for(const users::PsUser& user) -> CategoryPtr;
  /// @brief The held for-sale category for the user's audience.
  ///
  /// @param user The user.
  /// @return The category, or null when none is held or the audience is
  ///     unknown.
  auto for_sale_held_for(const users::PsUser& user) -> ForSalePtr;
  /// @brief Reports how the held answers were fetched, from held answers only.
  ///
  /// @param user The user.
  /// @return The report, or std::nullopt when nothing at all is held.
  auto how_fetched(users::PsUser& user) -> std::optional<FetchReport>;

 private:
  /// @brief Leads one fetch attempt and ends it on the board.
  ///
  /// @param user The user.
  /// @param stop Abandons the fetch.
  /// @param progress The attempt's reporting.
  /// @return As view.
  [[nodiscard]] auto led(users::PsUser& user, const Stop& stop,
                         const FetchProgress& progress)
      -> Expected<View, Unanswered>;
  /// @brief Builds the view step by step, reporting each step.
  ///
  /// @param user The user.
  /// @param stop Abandons the fetch.
  /// @param progress The attempt's reporting.
  /// @return As view.
  [[nodiscard]] auto built(users::PsUser& user, const Stop& stop,
                           const FetchProgress& progress)
      -> Expected<View, Unanswered>;
  /// @brief Reports the end of the owned library step.
  ///
  /// @param owned What the user owns.
  /// @param held Whether a whole library was held before the step.
  /// @param progress The attempt's reporting.
  static void report_library(const HeldAnswers::Owned& owned, bool held,
                             const FetchProgress& progress);
  /// @brief Reads one PS5 category as a step of the fetch.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  /// @tparam Held A callable taking the audience and returning the held
  ///     HeldCategory.
  /// @tparam Read A callable returning the category, as
  ///     ReadCategories::included and ReadCategories::for_sale do.
  /// @param user The user.
  /// @param step The step the category is read in.
  /// @param demand Whether the user asked to start over.
  /// @param progress The attempt's reporting.
  /// @param held Reads the held category.
  /// @param read Reads the category.
  /// @return As read.
  template <class Category, class Held, class Read>
  [[nodiscard]] auto category_step(const users::PsUser& user, FetchStep step,
                                   Demand demand, const FetchProgress& progress,
                                   const Held& held, const Read& read)
      -> Expected<std::shared_ptr<const Category>, Unanswered>;
  /// @brief Whether a shown game has a release date.
  ///
  /// @param games The games.
  /// @return True when a visible game's release has a time.
  static auto awaits_a_release(const MyGames& games) -> bool;
  /// @brief Whether a PS5 cloud game is shown.
  ///
  /// @param games The games.
  /// @return True when a visible game is on PS5 cloud streaming.
  static auto shows_a_ps5_game(const MyGames& games) -> bool;
  /// @brief Whether no source produced anything.
  ///
  /// @param list The LegacyCloudStream list.
  /// @param category The Premium category.
  /// @param owned What the user owns.
  /// @return True when there is no list, no category and no PS5 scan that was
  ///     read.
  static auto has_nothing_to_show(
      const Expected<ListPtr, Unanswered>& list,
      const Expected<CategoryPtr, Unanswered>& category,
      const HeldAnswers::Owned& owned) -> bool;
  /// @brief Makes sure the user's cloud streaming access is held, accepting a
  /// held one.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param progress Told the step's activity and outcome.
  /// @return Nothing, even when access could not be read; a cancellation.
  [[nodiscard]] auto streaming_for_tiles(users::PsUser& user, const Stop& stop,
                                         const FetchProgress& progress)
      -> Expected<void, Unanswered>;
  /// @brief The PS5 part of the fetch report, from held answers only.
  ///
  /// @param user The user.
  /// @param owned What the user owns, when held.
  /// @return The audience, categories, ownership, title count, trusted time and
  ///     exclusion snapshot held.
  auto ps5_report(users::PsUser& user,
                  const std::optional<HeldAnswers::Owned>& owned)
      -> FetchReport::CloudStream;

  /// @brief Sends every request; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief The trusted clock; may be null.
  TrustedClock* trusted_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
  /// @brief Reads and holds the library.
  ReadLibrary libraries_;
  /// @brief Reads and holds the streamable list.
  ReadStreamableList lists_;
  /// @brief Reads and holds the PS5 categories.
  ReadCategories categories_;
};

}  // namespace libchiaking::catalog::usecases
