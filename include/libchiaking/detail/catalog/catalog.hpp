/// @file
/// @brief Declares Catalog, the service that answers what a PlayStation user
/// may stream and whether a title may launch, with CloudCaptureFile and
/// RecentlyPlayedGame, two of the answers it gives.
#pragma once

#include "libchiaking/detail/catalog/call_failure.hpp"
#include "libchiaking/detail/catalog/cloud_access.hpp"
#include "libchiaking/detail/catalog/content_url.hpp"
#include "libchiaking/detail/catalog/explanation.hpp"
#include "libchiaking/detail/catalog/fetch_report.hpp"
#include "libchiaking/detail/catalog/fetch_state.hpp"
#include "libchiaking/detail/catalog/game_details.hpp"
#include "libchiaking/detail/catalog/kept_cache.hpp"
#include "libchiaking/detail/catalog/launch_check.hpp"
#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/library.hpp"
#include "libchiaking/detail/catalog/my_games.hpp"
#include "libchiaking/detail/catalog/ownership_answer.hpp"
#include "libchiaking/detail/catalog/played_game.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/store_page.hpp"
#include "libchiaking/detail/catalog/storefront_answer.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/catalog/streamable_list.hpp"
#include "libchiaking/detail/catalog/trusted_clock.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/users/request_budget.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class ThreadingProvider;
}

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
class RecordStore;
}  // namespace libchiaking::platform

namespace libchiaking::catalog {

/// @brief Where one capture in the account's cloud media gallery can be
/// downloaded.
///
/// The address carries its own access token, so it should be kept out of logs
/// and used soon.
///
/// @see Catalog::cloud_capture
/// @ingroup api_catalog
struct CloudCaptureFile {
  /// @brief The address of the full file.
  std::string url;
  /// @brief The file type, such as `jpg` or `mp4`; empty when PSN gave none.
  std::string file_type;
  /// @brief The size in bytes; 0 when PSN gave none.
  std::uint64_t size{};

  /// @brief Compares every field.
  friend auto operator==(const CloudCaptureFile&, const CloudCaptureFile&)
      -> bool = default;
};

/// @brief One of the user's shown cloud games that the account played
/// recently, as PSN's played titles list reports it.
///
/// Everything but the product and the last play comes from the played
/// release PSN listed and is empty when PSN gave nothing readable for it.
///
/// @see Catalog::recently_played
/// @ingroup api_catalog
struct RecentlyPlayedGame {
  /// @brief The listed product to show and stream.
  ProductId product;
  /// @brief When PSN last saw the title played; the epoch when PSN gave no
  /// readable time.
  Time last_played{};
  /// @brief The release PSN says was played, such as `PPSA01234_00`, which
  /// other PSN services, such as the trophy service, know the game by.
  std::optional<NpTitleId> title;
  /// @brief How long the account has played the release in all, to the
  /// second.
  std::optional<std::chrono::seconds> play_time;
  /// @brief When PSN first saw the release played.
  std::optional<Time> first_played;
  /// @brief How many times PSN saw the release started.
  std::optional<std::uint32_t> play_count;

  /// @brief Compares every field.
  friend auto operator==(const RecentlyPlayedGame&, const RecentlyPlayedGame&)
      -> bool = default;
};

/// @brief Answers which games a signed-in user can stream from the cloud and
/// whether one of them may launch now.
///
/// The catalog combines three things PS reports separately: the list of
/// titles that stream (StreamableList), the games the account owns (Library)
/// and the titles the account's PlayStation Plus plan includes. The combined
/// answer is MyGames. It covers both the older LegacyCloudStream service and
/// PS5 cloud streaming; see ServiceGeneration.
///
/// Calls that take a stop token block the calling thread while they talk to
/// PS and return when the answer is in. Calls without one only read what is
/// already held. Answers are kept in memory and, where noted, in the
/// platform::RecordStore, so a repeated question is usually answered without a
/// request. Several threads may call one Catalog; callers asking the same
/// question at the same time share one request.
///
/// A user who is signed out or whose sign-in has expired is turned away before
/// any request is made.
///
/// @see MyGames
/// @see LaunchVerdict
/// @see CallFailure
/// @ingroup api_catalog
class Catalog {
 public:
  /// @brief Not copyable; one catalog owns its held answers and in-flight
  /// requests.
  Catalog(const Catalog&) = delete;
  /// @brief Not copy-assignable; one catalog owns its held answers and
  /// in-flight requests.
  auto operator=(const Catalog&) -> Catalog& = delete;
  /// @brief Releases everything held in memory; records already written to
  /// the store stay there.
  ~Catalog();

  /// @brief Returns the titles PS streams in the user's store region.
  ///
  /// A held list is returned without a request when it is complete, did not
  /// come from the public store fallback and is under 24 hours old. Otherwise
  /// the public store of the account's region is walked without signing in,
  /// or the shared English store when the region has none. When that gives no
  /// titles, the account's LegacyCloudStream shelves are walked, and when that
  /// walk fails for a reason other than sign-in the public store is walked
  /// again as a fallback. The listed PS4 titles are then checked for a
  /// streaming copy a plan covers, and the PS5 store lists are added. The
  /// result is kept in memory and in the record store, keyed by region.
  ///
  /// @param user The signed-in user whose region and account are asked about.
  /// @param stop Cancels the walk.
  /// @return The list, shared with the catalog; it never changes once
  ///     returned. On failure, the reason no list could be built:
  ///     StreamableListReason::unavailable, StreamableListReason::rejected or
  ///     StreamableListReason::region_unsupported when PS gave no titles at
  ///     all.
  /// @retval StreamableListReason::cancelled The stop token fired.
  /// @retval StreamableListReason::user The user is signed out or must sign in
  ///     again; see CallFailure::cause.
  /// @retval StreamableListReason::malformed PS's reply could not be read, or
  ///     the titles it returned repeat a product.
  [[nodiscard]] auto get_streamable_list(users::PsUser& user,
                                         const platform::StopToken& stop = {})
      -> Expected<std::shared_ptr<const StreamableList>, StreamableListFailure>;
  /// @brief Returns the games the account owns.
  ///
  /// A library fetched less than 24 hours ago is returned without a request.
  /// Otherwise every entitlement record is fetched, records that are not
  /// games are dropped, and the result is kept in memory and in the record
  /// store. Two further questions are asked when they apply, the concepts of
  /// streamed PS4 copies and whether each PS5 copy streams; if either fails
  /// the library is returned without that detail.
  ///
  /// @param user The signed-in user whose account is asked about.
  /// @param stop Cancels the fetch.
  /// @return The library, shared with the catalog; it never changes once
  ///     returned.
  /// @retval LibraryReason::cancelled The stop token fired.
  /// @retval LibraryReason::user The user's session could not make the
  ///     call; see CallFailure::cause.
  /// @retval LibraryReason::malformed An entitlement reply could not be read.
  [[nodiscard]] auto library_of(users::PsUser& user,
                                const platform::StopToken& stop = {})
      -> Expected<std::shared_ptr<const Library>, LibraryFailure>;
  /// @brief Builds the user's games by matching the streamable list against
  /// the library and the plan.
  ///
  /// Loads the streamable list, the library and the titles included with the
  /// plan, each from what is held when fresh, then decides for every title
  /// how it may be played and whether it is shown. This is the call a library
  /// screen makes.
  ///
  /// A library that cannot be fetched does not fail the call unless the cause
  /// is cancellation or sign-in: the games are then built with
  /// MyGames::library_state unavailable and unowned PS5 titles report
  /// OwnershipState::unknown. The included titles are likewise optional.
  ///
  /// One fetch runs per account at a time. A call made while one runs joins
  /// it: it is told the fetch's current state at once and gets the same games.
  /// Its stop token ends only its own wait; when the caller leading the fetch
  /// is cancelled, a joining caller carries on into a new fetch. A call made
  /// after Catalog::start_over never gets the games of a fetch that began
  /// before it: it watches the running fetch to its end, then leads a new one
  /// and returns that fetch's games. Every step of the fetch is kept as
  /// FetchState; see Catalog::fetch_state.
  ///
  /// @param user The signed-in user to build the games for.
  /// @param stop Cancels any request still needed.
  /// @param observer Told every state of the fetch this call leads or joins,
  ///     in order, on the thread running the fetch, ending with
  ///     FetchStep::ready, FetchStep::unanswered or, for the caller that led
  ///     it, FetchStep::cancelled. Never called after this returns; may be
  ///     empty. See FetchObserver for what it may do.
  /// @return The games, shared with the catalog. The same object is returned
  ///     again while its MyGames::Inputs are unchanged.
  /// @retval GamesReason::cancelled The stop token fired.
  /// @retval GamesReason::user The user is signed out or must sign in
  ///     again.
  /// @retval GamesReason::unavailable No streamable list could be
  ///     built.
  /// @retval GamesReason::rejected No streamable list could be built because
  ///     PS refused the request.
  /// @retval GamesReason::region_unsupported No streamable list could be built
  ///     because the region has no store.
  /// @retval GamesReason::malformed No streamable list could be built because
  ///     PS's reply could not be read.
  [[nodiscard]] auto process_library_and_list(
      users::PsUser& user, const platform::StopToken& stop = {},
      const FetchObserver& observer = {})
      -> Expected<std::shared_ptr<const MyGames>, GamesFailure>;
  /// @brief Reads where the user's latest full fetch stands.
  ///
  /// Catalog::process_library_and_list and the loads that
  /// Catalog::clear_for_launch makes run as full fetches; this reports the
  /// latest of them, finished or running, so a screen opened part way
  /// through shows the real step. Catalog::start_over and Catalog::forget
  /// return an ended fetch to FetchStep::idle. Makes no request.
  ///
  /// @param user The user whose fetch is meant.
  /// @return The latest state; FetchStep::idle before any fetch.
  auto fetch_state(const users::PsUser& user) const -> FetchState;
  /// @brief Lists the user's shown cloud games that the account played most
  /// recently.
  ///
  /// Builds the user's games as Catalog::process_library_and_list does, then
  /// asks PSN for the account's most recently played titles and keeps those
  /// whose PS4 or PS5 title number matches a shown game, in PSN's order.
  /// PS3 titles have no such number and are never listed. Games the user
  /// could only buy, Standing::store, are left out; games PS's list marks
  /// as unstreamable, Standing::may_stream, are kept for the caller to check
  /// against the storefront.
  ///
  /// Each game carries what PSN reports about the played release it was
  /// matched by: its title id, total play time, first play and play count.
  ///
  /// @param user The signed-in user whose play history to read.
  /// @param stop Cancels any request still needed.
  /// @return At most twenty games, most recently played first, each listed
  ///     once.
  /// @retval RecentlyPlayedReason::cancelled The stop token fired.
  /// @retval RecentlyPlayedReason::user The user is signed out, must sign in
  ///     again, or PSN did not return the played titles; see
  ///     CallFailure::cause.
  /// @retval RecentlyPlayedReason::unavailable The user's games could not be
  ///     built; the failures of Catalog::process_library_and_list apply, and
  ///     RecentlyPlayedReason::rejected,
  ///     RecentlyPlayedReason::region_unsupported and
  ///     RecentlyPlayedReason::malformed carry them.
  [[nodiscard]] auto recently_played(users::PsUser& user,
                                     const platform::StopToken& stop = {})
      -> Expected<std::vector<RecentlyPlayedGame>, RecentlyPlayedFailure>;
  /// @brief Lists every game the account has played, on any console and
  /// whether or not it streams, with PSN's play time for each.
  ///
  /// A history read under six hours ago is returned without a request, from
  /// memory or the platform::RecordStore. Otherwise PSN's game list is read
  /// a page of 50 titles at a time until it gives no next page, and the whole
  /// history is kept for the account. Each page waits its turn on the
  /// users::RequestBudget the catalog was given, so it counts against the
  /// same per-account budget as every service sharing it. Titles PSN sent
  /// without a readable title id are left out, and a title listed twice is kept
  /// once. Catalog::start_over and Catalog::forget discard what is kept.
  ///
  /// @param user The signed-in user whose play history to read.
  /// @param stop Cancels any request still needed.
  /// @return The games in PSN's order, most recently played first.
  /// @retval PlayedGamesReason::cancelled The stop token fired.
  /// @retval PlayedGamesReason::user The user is signed out, must sign in
  ///     again, or PSN did not return a page; see CallFailure::cause, which
  ///     is users::Unanswered::Reason::rate_limited when PSN answered 429
  ///     or the account's request budget is spent.
  /// @retval PlayedGamesReason::malformed A page's next offset did not move
  ///     past the one asked for, or fifty pages were read without an end.
  [[nodiscard]] auto played_games(users::PsUser& user,
                                  const platform::StopToken& stop = {})
      -> Expected<std::vector<PlayedGame>, PlayedGamesFailure>;
  /// @brief Finds where a screenshot or clip a PS5 cloud console uploaded to
  /// the account's cloud media gallery can be downloaded.
  ///
  /// The id is the one SessionEvents::on_capture_uploaded reports. The
  /// address returned carries its own access token.
  ///
  /// @param user The signed-in user whose gallery holds the capture.
  /// @param gallery_id The capture's id in the gallery.
  /// @param stop Cancels the request.
  /// @return Where to download the capture.
  /// @retval CloudCaptureReason::cancelled The stop token fired.
  /// @retval CloudCaptureReason::user The user is signed out, must sign in
  ///     again, or PSN did not name the capture; see CallFailure::cause.
  [[nodiscard]] auto cloud_capture(users::PsUser& user,
                                   std::string_view gallery_id,
                                   const platform::StopToken& stop = {})
      -> Expected<CloudCaptureFile, CloudCaptureFailure>;
  /// @brief Reads whether the account wants screenshots and clips taken in
  /// PS5 cloud games downloaded to the device streaming them, as set in the
  /// PS5's capture settings.
  ///
  /// @param user The signed-in user whose setting to read.
  /// @param stop Cancels the request.
  /// @return Whether the account chose to download them, or PSN's default
  ///     when it never chose.
  /// @retval CaptureDownloadReason::cancelled The stop token fired.
  /// @retval CaptureDownloadReason::user The user is signed out, must sign in
  ///     again, or PSN did not answer; see CallFailure::cause.
  [[nodiscard]] auto cloud_capture_download(
      users::PsUser& user, const platform::StopToken& stop = {})
      -> Expected<bool, CaptureDownloadFailure>;
  /// @brief Asks the store, as this account, whether one PS5 cloud title
  /// streams and whether it can be added to the library.
  ///
  /// An answer under five minutes old is reused; answers are kept in memory
  /// only. The games are not loaded for this call: the product is looked up
  /// in what is already held.
  ///
  /// @param user The signed-in user to ask as.
  /// @param product The title, by its listed product id or an alias.
  /// @param stop Cancels the request.
  /// @return The store's answer for the edition that matches the product.
  /// @retval StorefrontReason::rejected The title streams on the older PS
  ///     Now service, which the storefront is not asked about, or the store
  ///     refused the question.
  /// @retval StorefrontReason::user The user is signed out, or store access
  ///     has lapsed.
  /// @retval StorefrontReason::cancelled The stop token fired.
  /// @retval StorefrontReason::malformed The store's reply could not be
  ///     read.
  /// @retval StorefrontReason::not_listed The store does not know the
  ///     product.
  /// @retval StorefrontReason::unavailable The store could not be reached.
  [[nodiscard]] auto check_title_against_storefront(
      users::PsUser& user, const ProductId& product,
      const platform::StopToken& stop = {})
      -> Expected<StorefrontAnswer, StorefrontFailure>;
  /// @brief Asks PS again whether the user owns one listed title.
  ///
  /// Meant for the moment after a purchase or an add to library, when the
  /// held library is out of date. For a PS5 cloud title the entitlement for
  /// that title alone is fetched and merged into the held library; if none is
  /// found the whole library is fetched again. A held storefront answer for
  /// the title is dropped and the games are rebuilt. For a title on the older
  /// LegacyCloudStream service the answer comes from the loaded games without a
  /// further request.
  ///
  /// @param user The signed-in user to ask as.
  /// @param product The title, by its listed product id or an alias.
  /// @param stop Cancels any request.
  /// @return Whether the title is owned, and the rebuilt MyGame when it is.
  /// @retval OwnershipRecheckReason::not_listed The product is not among the
  ///     user's games.
  /// @retval OwnershipRecheckReason::ownership_unknown A LegacyCloudStream
  /// title was asked
  ///     about while the library could not be read.
  /// @retval OwnershipRecheckReason::cancelled The stop token fired.
  /// @retval OwnershipRecheckReason::user The user's session could not make the
  ///     call.
  /// @retval OwnershipRecheckReason::malformed PS's entitlement reply could not
  ///     be read.
  /// @retval OwnershipRecheckReason::unavailable The user's games could not be
  ///     built; OwnershipRecheckReason::rejected and
  ///     OwnershipRecheckReason::region_unsupported likewise carry the
  ///     failures of Catalog::process_library_and_list.
  [[nodiscard]] auto recheck_ownership(users::PsUser& user,
                                       const ProductId& product,
                                       const platform::StopToken& stop = {})
      -> Expected<OwnershipAnswer, OwnershipRecheckFailure>;
  /// @brief Judges from held answers alone whether a title may launch; makes
  /// no request.
  ///
  /// Use it to enable or disable a play control. Nothing is loaded: when no
  /// list or library is held yet the verdict is
  /// LaunchRefusal::Kind::not_listed. The membership tier and the storefront
  /// are not asked, and a missing trusted time does not refuse here.
  /// Catalog::clear_for_launch makes the full check. A PS5 Premium bundle's
  /// own product is refused with LaunchRefusal::Kind::choose_a_game; ask
  /// with one of MyGame::children instead.
  ///
  /// @param user The user who wants to play.
  /// @param product The title, by its listed product id or an alias.
  /// @return An allowed verdict carrying a ClearedLaunch, or the first
  ///     refusal that applies. A signed-out or expired user gets
  ///     LaunchRefusal::Kind::check_unavailable.
  /// @see LaunchVerdict::before_launch
  auto can_launch(users::PsUser& user, const ProductId& product) const
      -> LaunchVerdict;
  /// @brief Makes every check needed before a stream is started and returns
  /// what to launch.
  ///
  /// Loads the games, then checks a PS5 cloud title in order: the game's
  /// standing in the list, the account's cloud streaming access,
  /// ownership (asked again when it is unknown or the title still has to be
  /// added), whether the owned PS5 entitlement still exists and streams, the
  /// cloud version of the title, the list of excluded titles, and the release
  /// date against trusted time. A title on the older LegacyCloudStream
  /// service is checked for its standing, the membership tier and its release
  /// date only. The storefront is not asked here; see
  /// Catalog::check_title_against_storefront. A PS5 Premium bundle is never
  /// launched itself: its own product is refused with
  /// LaunchRefusal::Kind::choose_a_game before any request, and a child asked
  /// by its product launches through its own entitlement, or is refused with
  /// the add-to-library offer when that entitlement is not owned.
  ///
  /// @param user The user who wants to play.
  /// @param product The title, by its listed product id or an alias.
  /// @param stop Cancels any request.
  /// @param observer Told, on this thread, as each LaunchCheck starts; may be
  ///     empty to follow nothing.
  /// @return An allowed verdict whose LaunchVerdict::cleared_launch says what
  ///     to launch, or a refusal. A check that could not be completed,
  ///     cancellation included, comes back as
  ///     LaunchRefusal::Kind::check_unavailable with the cause.
  /// @see Catalog::can_launch
  /// @see Catalog::remember_refusal
  auto clear_for_launch(users::PsUser& user, const ProductId& product,
                        const platform::StopToken& stop = {},
                        const LaunchCheckObserver& observer = {})
      -> LaunchVerdict;
  /// @brief Records that PS's streaming service refused to start a title.
  ///
  /// The refusal is kept for 24 hours in memory and in the record store,
  /// together with the user's tier and whether the title was held. While it
  /// stands, the title's MyGame carries it and launch verdicts refuse with
  /// LaunchRefusal::Kind::refused_by_service. A refusal of kind
  /// StreamRefusal::Kind::no_game_for_entitlement also drops the held library
  /// so the next load fetches it again. A StreamRefusal::Kind::trial_ended
  /// refusal, which the PS5 launch checks record themselves, lists the trial
  /// as ended (OwnedTitle::trial_ended) and refuses with
  /// LaunchRefusal::Kind::purchase_needed instead.
  ///
  /// Nothing is recorded for a user who is signed out or whose sign-in has
  /// expired. A failure to write the record is not reported.
  ///
  /// @param user The user who was refused.
  /// @param product The title that was refused.
  /// @param kind What the streaming service said.
  /// @see StreamRefusal
  void remember_refusal(users::PsUser& user, const ProductId& product,
                        StreamRefusal::Kind kind);
  /// @brief Finds the PlayStation Store web page to open for a title.
  ///
  /// The product's own page is preferred and is checked with one request;
  /// when it does not exist the concept page of the listed title is returned
  /// instead. The page is in the locale of the held list, or the account's
  /// region when no list is held.
  ///
  /// @param user The signed-in user whose region decides the store.
  /// @param product The title to open.
  /// @param stop Cancels the request.
  /// @return The page to open.
  /// @retval StorePageReason::user The user is signed out or must sign in
  ///     again.
  /// @retval StorePageReason::cancelled The stop token fired.
  /// @retval StorePageReason::not_listed Neither a product page nor a
  ///     concept page exists; CallFailure::http_status carries the store's
  ///     status when it refused.
  [[nodiscard]] auto store_page_of(users::PsUser& user,
                                   const ProductId& product,
                                   const platform::StopToken& stop = {})
      -> Expected<StorePage, StorePageFailure>;
  /// @brief Fetches the store's art, screenshots, trailers, ratings and facts
  /// for a product.
  ///
  /// Details under 24 hours old are served from the record store. Otherwise
  /// one request asks the store's catalog for the product, stating the
  /// account's age, and the answer is kept under the product. The store does
  /// not list a product rated above that age, and a product is not listed
  /// either when the account states no age. When the catalog does not list the
  /// product and the held games or library name a concept for it, the details
  /// are fetched for that concept instead, as the concept overload does.
  ///
  /// @param user The signed-in user whose region decides the locale and
  ///     whose account states the age.
  /// @param product The product to describe.
  /// @param stop Cancels the requests.
  /// @return The details, with GameDetails::product set to the product asked
  ///     about and GameDetails::concept_id set only when the concept
  ///     answered.
  /// @retval ProductDetailsReason::user The user is signed out or must sign in
  ///     again.
  /// @retval ProductDetailsReason::cancelled The stop token fired.
  /// @retval ProductDetailsReason::not_listed The store does not know the
  ///     product.
  /// @retval ProductDetailsReason::malformed The store's reply could not be
  ///     read.
  /// @retval ProductDetailsReason::rejected The store refused the question.
  /// @retval ProductDetailsReason::unavailable The store could not be reached.
  [[nodiscard]] auto details_of(users::PsUser& user, const ProductId& product,
                                const platform::StopToken& stop = {})
      -> Expected<GameDetails, ProductDetailsFailure>;
  /// @brief Fetches the store's art, screenshots, trailers, ratings and facts
  /// for a concept.
  ///
  /// Details under 24 hours old are served from the record store. Otherwise
  /// four requests ask the public store for the concept's art, ratings and
  /// facts. The art request must succeed; the others are optional, and
  /// details missing any of them are returned but not kept. The user's
  /// sign-in state is not checked and GameDetails::product is left empty.
  ///
  /// @param user The user whose region decides the locale.
  /// @param grouped_under The concept to describe.
  /// @param stop Cancels the requests.
  /// @return The details for the concept.
  /// @retval ConceptDetailsReason::cancelled The stop token fired.
  /// @retval ConceptDetailsReason::not_listed The store does not know the
  ///     concept.
  /// @retval ConceptDetailsReason::malformed The store's reply could not be
  ///     read.
  /// @retval ConceptDetailsReason::rejected The store refused the question.
  /// @retval ConceptDetailsReason::unavailable The store could not be reached.
  [[nodiscard]] auto details_of(users::PsUser& user,
                                const ConceptId& grouped_under,
                                const platform::StopToken& stop = {})
      -> Expected<GameDetails, ConceptDetailsFailure>;
  /// @brief Returns the bytes of a store image, from the image cache when
  /// held.
  ///
  /// A fetched image is written to the record store; if that write fails the
  /// bytes are still returned. No user is needed because the images are
  /// public.
  ///
  /// @param image The image to load.
  /// @param stop Cancels the download.
  /// @return The encoded image file as PS serves it; decoding is left to
  ///     the caller.
  /// @retval ImageReason::cancelled The stop token fired.
  /// @retval ImageReason::not_listed The image does not exist.
  /// @retval ImageReason::rejected PS refused the request, or the
  ///     image is larger than ImageHost::largest_bytes.
  /// @retval ImageReason::malformed The reply was empty.
  /// @retval ImageReason::unavailable The image host could not be
  ///     reached.
  [[nodiscard]] auto image_of(const ImageRef& image,
                              const platform::StopToken& stop = {})
      -> Expected<std::vector<std::byte>, ImageFailure>;
  /// @brief Reports whether the account can reach the older LegacyCloudStream
  /// streaming service.
  ///
  /// An answer under 15 minutes old, taken for the same sign-in state, is
  /// returned from memory. Otherwise the service's shelves are walked. A walk
  /// that fails is not an error here: it is reported through
  /// CloudAccess::availability.
  ///
  /// @param user The signed-in user to check.
  /// @param stop Cancels the walk.
  /// @return What the service showed for this account.
  /// @retval CloudAccessReason::user The user is signed out or must sign in
  ///     again.
  /// @retval CloudAccessReason::cancelled The stop token fired.
  [[nodiscard]] auto cloud_access_of(users::PsUser& user,
                                     const platform::StopToken& stop = {})
      -> Expected<CloudAccess, CloudAccessFailure>;
  /// @brief Says which rules listed, hid or dropped a product in the held
  /// games; makes no request.
  ///
  /// @param user The user whose games are meant.
  /// @param product The product to explain, by its listed id or an alias.
  /// @return The steps recorded when the games were built. When nothing is
  ///     held, the user is signed out, or the product was never considered,
  ///     the result is Explanation::not_listed.
  auto explain(users::PsUser& user, const ProductId& product) const
      -> Explanation;
  /// @brief The PS5 cloud games the user holds as a trial or demo, from held
  /// answers alone; makes no request.
  ///
  /// The held answers include the library record kept on disk, so this
  /// answers after a restart without the library having been fetched in
  /// this run.
  ///
  /// @param user The user whose games are meant.
  /// @return The shown games held as trial access whose trial has not
  ///     ended, in the held games' order; empty when nothing is held or the
  ///     user is signed out.
  auto held_trials(users::PsUser& user) const -> std::vector<MyGame>;
  /// @brief Reads the trusted time this catalog judges release dates by.
  ///
  /// Asks the TrustedClock given to the constructor without syncing it, so
  /// a release countdown shown to the player runs on the same clock as
  /// can_launch and clear_for_launch. Makes no request and never blocks on
  /// the network; the clock is synced when games with a release date are
  /// loaded and when a launch is cleared.
  ///
  /// @return The trusted time, or nothing when the catalog was built without
  ///     a TrustedClock or the clock holds no usable time.
  auto trusted_time() const noexcept -> std::optional<Time>;
  /// @brief Summarises where the held list, library and plan data came from,
  /// for a diagnostics screen; makes no request.
  ///
  /// @param user The user whose held answers are meant.
  /// @return The report, or nothing when the user is signed out, has no
  ///     usable region, or no streamable list is held for the region.
  auto how_fetched(users::PsUser& user) const -> std::optional<FetchReport>;
  /// @brief Discards what is held for the user so the next load asks PS
  /// again.
  ///
  /// Drops the account's library, games, storefront answers, cloud access and
  /// play history from memory, the library and play history records from the
  /// store, and the streamable list
  /// for the user's region from both. The next
  /// Catalog::process_library_and_list also fetches the plan's included
  /// titles again. Remembered refusals, game details and images are kept.
  ///
  /// A fetch already running for the user is left to end, and its callers
  /// get its games. Catalog::process_library_and_list called after this
  /// returns does not take them: it waits for that fetch to end and then
  /// fetches afresh, dropping anything the earlier fetch kept after the
  /// start-over.
  ///
  /// @param user The user to start over for.
  void start_over(users::PsUser& user);
  /// @brief Removes everything held about an account, for sign-out or account
  /// removal.
  ///
  /// Drops the account's answers from memory and its records, remembered
  /// refusals included, from the store. Streamable lists, game details and
  /// images are shared between accounts and are kept. Failures to erase are
  /// not reported.
  ///
  /// @param account The account to forget.
  void forget(AccountId account);

  /// @brief Lists every cache with its entry count and size, for a storage
  /// screen.
  ///
  /// @return One summary per CacheId, in the order of every_cache.
  /// @retval CachesReason::storage The record store could not be
  ///     listed.
  [[nodiscard]] auto caches()
      -> Expected<std::vector<CacheSummary>, CachesFailure>;
  /// @brief Lists the records one cache holds.
  ///
  /// @param cache The cache to list.
  /// @return One entry per record.
  /// @retval CacheEntriesReason::storage The record store could not be read.
  [[nodiscard]] auto cache_entries(CacheId cache)
      -> Expected<std::vector<CacheEntry>, CacheEntriesFailure>;
  /// @brief Empties one cache, in memory and in the record store.
  ///
  /// Clearing CacheId::game_details also drops the images those details show.
  ///
  /// @param cache The cache to empty.
  /// @return Nothing on success.
  /// @retval ClearCacheReason::storage A record could not be listed or
  ///     erased.
  [[nodiscard]] auto clear_cache(CacheId cache)
      -> Expected<void, ClearCacheFailure>;
  /// @brief Empties every cache.
  ///
  /// Every cache is attempted even when an earlier one fails.
  ///
  /// @return Nothing when all were emptied; otherwise the failure of the last
  ///     cache that could not be cleared.
  /// @retval ClearCachesReason::storage A record could not be listed or
  ///     erased.
  [[nodiscard]] auto clear_caches() -> Expected<void, ClearCachesFailure>;

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Builds a catalog that judges release dates against a clock the
  /// caller supplies.
  ///
  /// @param http Sends every request to PS. Must outlive the catalog.
  /// @param cache_location Where lists, libraries, refusals, game details and
  ///     images are kept between runs. Must outlive the catalog.
  /// @param threading Makes the catalog's locks, the waits of requests
  ///     shared between callers and the threads details are read on. Must
  ///     outlive the catalog.
  /// @param clock Monotonic clock those waits measure on. Must outlive the
  ///     catalog.
  /// @param wall_clock Device time, used to stamp answers and to decide when
  ///     a held answer has gone stale. Must outlive the catalog.
  /// @param trusted_clock Time that the user cannot change, used only to
  ///     decide whether a game has been released. Must outlive the catalog.
  /// @param budget The per-account request budget Catalog::played_games is
  ///     paced on, shared with other services such as the trophies. Must
  ///     outlive the catalog.
  /// @param diagnostics Receives log lines; may be null to log nothing. Must
  ///     outlive the catalog when given.
  Catalog(platform::HttpProvider& http, platform::RecordStore& cache_location,
          platform::ThreadingProvider& threading,
          const platform::ClockProvider& clock,
          const platform::ClockProvider& wall_clock,
          TrustedClock& trusted_clock, users::RequestBudget& budget,
          platform::DiagnosticsProvider* diagnostics = nullptr);
  /// @brief Builds a catalog with no trusted time.
  ///
  /// Release dates are never judged against PS's or the device's clock, so
  /// every game that carries a release date is refused as having no trusted
  /// time; games without one are unaffected. Use the constructor taking a
  /// TrustedClock to launch dated games.
  ///
  /// @param http Sends every request to PS. Must outlive the catalog.
  /// @param cache_location Where lists, libraries, refusals, game details and
  ///     images are kept between runs. Must outlive the catalog.
  /// @param threading Makes the catalog's locks, the waits of requests
  ///     shared between callers and the threads details are read on. Must
  ///     outlive the catalog.
  /// @param clock Monotonic clock those waits measure on. Must outlive the
  ///     catalog.
  /// @param wall_clock Device time, used to stamp answers and to decide when
  ///     a held answer has gone stale. Must outlive the catalog.
  /// @param budget The per-account request budget Catalog::played_games is
  ///     paced on, shared with other services such as the trophies. Must
  ///     outlive the catalog.
  /// @param diagnostics Receives log lines; may be null to log nothing. Must
  ///     outlive the catalog when given.
  Catalog(platform::HttpProvider& http, platform::RecordStore& cache_location,
          platform::ThreadingProvider& threading,
          const platform::ClockProvider& clock,
          const platform::ClockProvider& wall_clock,
          users::RequestBudget& budget,
          platform::DiagnosticsProvider* diagnostics = nullptr);

  class Holdings;

  std::unique_ptr<Holdings> holdings_;
};

}  // namespace libchiaking::catalog
