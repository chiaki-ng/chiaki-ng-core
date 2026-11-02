/// @file
/// @brief Declares LibraryEntitlements, the app library: the entitlement
/// service the app reads the account's owned games from
///
/// It answers three questions: which concepts the account owns of one package
/// type (streamed PS4 games for LegacyCloudStream), every owned PS5 game that
/// streams to this kind of device, and whether one PS5 entitlement is still
/// owned at launch.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief An owned entitlement reduced to its concept. Plain data.
struct EntitlementConcept {
  /// @brief The entitlement `id`; may be empty when the product id is not.
  std::string id;
  /// @brief The `productId`; may be empty when the id is not.
  std::string product_id;
  /// @brief The `conceptMeta.conceptId` in decimal; never empty.
  std::string concept_id;
};

/// @brief The concepts the account owns of one package type. Plain data.
struct EntitlementConcepts {
  /// @brief The entitlements that name a numeric concept and an id or product
  /// id, in reply order.
  std::vector<EntitlementConcept> known;
  /// @brief The `totalResults` the service stated.
  std::size_t total{};
  /// @brief How many entitlements were received, usable or not.
  std::size_t received{};
  /// @brief How many pages were requested.
  int requests{};
  /// @brief Whether the walk stopped at the page limit before the end.
  bool truncated{};
};

/// @brief An owned PS5 game entitlement with the metadata the catalogue needs.
/// Plain data. Text fields are empty and optional fields nothing when PS
/// omitted them or sent the wrong type.
struct GameEntitlement {
  /// @brief The entitlement `id`; never empty.
  std::string id;
  /// @brief The `productId`.
  std::string product_id;
  /// @brief The `skuId`.
  std::string sku_id;
  /// @brief The `activeFlag`.
  std::optional<bool> active{};
  /// @brief The `activeDate` as PS wrote it; empty when it is not a string.
  std::optional<std::string> active_date{};
  /// @brief The `entitlementType`.
  std::optional<std::int64_t> entitlement_type{};
  /// @brief The `featureType`.
  std::optional<std::int64_t> feature_type{};
  /// @brief The `preorderFlag`.
  std::optional<bool> preorder{};
  /// @brief The `preorderCoreFlag`, which gates launch on the release time.
  std::optional<bool> preorder_core{};
  /// @brief The `preorderPlaceholderFlag`, which also gates launch on the
  /// release time.
  std::optional<bool> preorder_placeholder{};
  /// @brief The `streamingSupported` flag.
  std::optional<bool> streaming_supported{};
  /// @brief The `isPlayTimeFullyConsumed` flag, read at the top level or
  /// in `gameMeta`, which a PS5 checks before streaming a Play First Trial.
  std::optional<bool> play_time_used_up{};
  /// @brief The `skuMeta.playabilityDate`; empty when it is not a string.
  std::optional<std::string> playability_date{};
  /// @brief The `gameMeta.packageType`, such as `PSGD`.
  std::string package_type{};
  /// @brief The `gameMeta.name`.
  std::string game_name{};
  /// @brief The `gameMeta.iconUrl`.
  std::string game_icon_url{};
  /// @brief The `titleMeta.titleId`, such as `PPSA01234_00`.
  std::string title_id{};
  /// @brief The `titleMeta.name`.
  std::string title_name{};
  /// @brief The `titleMeta.imageUrl`.
  std::string title_image_url{};
  /// @brief The `conceptMeta.conceptId` in decimal; empty when not numeric.
  std::string concept_id{};
  /// @brief The `conceptMeta.name`.
  std::string concept_name{};
  /// @brief The `conceptMeta.iconUrl`.
  std::string concept_icon_url{};
  /// @brief The `productMeta.imageUrl`.
  std::string product_image_url{};

  /// @brief Compares every field.
  ///
  /// @return Whether all fields are equal.
  friend auto operator==(const GameEntitlement&, const GameEntitlement&)
      -> bool = default;
};

/// @brief Every owned PS5 game that streams to this device. Plain data.
struct GameEntitlements {
  /// @brief The games with an id, from the last pass, in reply order.
  std::vector<GameEntitlement> entitlements;
  /// @brief The last pass's stated `totalResults`.
  std::size_t total{};
  /// @brief How many entitlements the last pass received.
  std::size_t received{};
  /// @brief How many pages were requested across all passes.
  int requests{};
  /// @brief How many times the walk restarted because the `revisionId`
  /// changed.
  int revision_restarts{};
  /// @brief Whether the `revisionId` was still changing after
  /// revision_restart_limit restarts, so the list may be inconsistent.
  bool revision_unsettled{};
  /// @brief Whether the last pass stopped at the page limit before the end.
  bool truncated{};
};

/// @brief Told as each page of an app library walk lands, with the
/// number of pages the walk holds on the first page of each pass.
using LibraryPageObserver =
    std::function<void(std::optional<std::size_t> pages_in_walk)>;

/// @brief Reads the app library with the app token from
/// `m.np.playstation.com/api/entitlement/v2/users/me/internal/entitlements`.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for its requests, 100 ms apart between pages. Not for
/// concurrent use.
class LibraryEntitlements {
 public:
  /// @brief The failure of owned_ps5_game().
  using OwnedGameError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of owned_ps5_games().
  using OwnedGamesError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of games_pass().
  using GamesPassError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of concepts_of().
  using ConceptsError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of a walk().
  using WalkError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The concepts walk's diagnostic label.
  static constexpr std::string_view label = "psn.entitlement.library_concepts";
  /// @brief The PS5 games walk's diagnostic label.
  static constexpr std::string_view games_label =
      "psn.entitlement.library_games";
  /// @brief The single PS5 game's diagnostic label.
  static constexpr std::string_view game_label = "psn.entitlement.library_game";
  /// @brief The package type of PS4 games streamed through LegacyCloudStream.
  static constexpr std::string_view streamed_ps4_games = "PS4GS";
  /// @brief The package type of PS5 games.
  static constexpr std::string_view ps5_games = "PSGD";
  /// @brief How many entitlements a concepts page asks for.
  static constexpr std::size_t page_size = 20;
  /// @brief How many entitlements a PS5 games page asks for.
  static constexpr std::size_t games_page_size = 100;
  /// @brief The most pages one walk reads.
  static constexpr std::size_t page_limit = 25;
  /// @brief The most times the PS5 games walk restarts on a changed revision.
  static constexpr int revision_restart_limit = 2;
  /// @brief The error code PS answers when the account does not own the
  /// entitlement asked for.
  static constexpr std::int64_t not_owned_code = 3161955;
  /// @brief The wait between pages and before a restart.
  static constexpr std::chrono::milliseconds page_delay{100};
  /// @brief The page size the pager is told, so any non-empty page continues
  /// the walk; it ends when the stated total is reached or a page is empty.
  static constexpr int fewest_rows_to_continue = 1;

  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param token The app token.
  LibraryEntitlements(net::http::Client& client,
                      auth::BearerToken token) noexcept;

  /// @brief Lists the concepts the account owns of one package type.
  ///
  /// GET
  /// `...?isActive=true&gameMetaPackageType=<type>&limit=20&offset=<n>&fields=conceptMeta,titleMeta`,
  /// page by page until `totalResults` is reached.
  ///
  /// @param package_type The package type, such as streamed_ps4_games.
  /// @param observe Told each page as it lands; may be empty.
  /// @return The concepts.
  /// @retval bad_reply::InvalidSchema A page lacks `entitlements` or
  ///     `totalResults`.
  /// @retval Error A transport failure (cancelled included), an error status
  ///     or an unreadable page.
  [[nodiscard]] auto concepts_of(std::string_view package_type,
                                 const LibraryPageObserver& observe = {})
      -> Expected<EntitlementConcepts, ConceptsError>;
  /// @brief Lists every active PS5 game the account owns, each with whether
  /// it streams.
  ///
  /// GET
  /// `...?isActive=true&gameMetaPackageType=PSGD&sortBy=ACTIVE_DATE&
  /// sortDirection=asc&limit=100&offset=<n>&useCache=true&fields=titleMeta,
  /// gameMeta,productMeta,conceptMeta,streamingSupported,skuMeta,preorderFlag,
  /// preorderCoreFlag`. The query does not send
  /// `streamingSupportedForDevice=true`: it filters by the requesting
  /// device, and the app token names a phone, so it hid every copy;
  /// each copy's `streamingSupported` says whether it streams instead. When a
  /// page's `revisionId` differs from the first page's, the library changed
  /// mid-walk and the walk starts again, at most revision_restart_limit times.
  ///
  /// @param observe Told each page as it lands, the page total again on each
  ///     restart; may be empty.
  /// @return The games; revision_unsettled is set when the revision never
  ///     settled.
  /// @retval bad_reply::InvalidSchema A page lacks `entitlements` or
  ///     `totalResults`.
  /// @retval Error A transport failure (cancelled included, also while
  ///     waiting to restart), an error status or an unreadable page.
  [[nodiscard]] auto owned_ps5_games(const LibraryPageObserver& observe = {})
      -> Expected<GameEntitlements, OwnedGamesError>;
  /// @brief Re-reads one owned PS5 entitlement, as a launch check does.
  ///
  /// GET `.../entitlements/<id>?fields=gameMeta,titleMeta,productMeta,
  /// conceptMeta,streamingSupported,skuMeta,preorderCoreFlag`.
  ///
  /// @param id The entitlement id.
  /// @return The entitlement; nothing for 404, or for an error status whose
  ///     body carries code not_owned_code.
  /// @retval bad_reply::InvalidSchema The reply has no entitlement id.
  /// @retval Error A transport failure, another error status or an unreadable
  ///     reply.
  [[nodiscard]] auto owned_ps5_game(std::string_view id)
      -> Expected<std::optional<GameEntitlement>, OwnedGameError>;

 private:
  /// @brief A page's `entitlements`, `totalResults` and `revisionId`.
  struct Reply;
  /// @brief An error reply's code, top-level or under `error`.
  struct Refusal;

  /// @brief Which list a walk reads.
  enum class Asked : std::uint8_t {
    /// @brief The concepts of one package type.
    concepts,
    /// @brief The PS5 games that stream to this device.
    games
  };

  /// @brief What a walk saw.
  struct Walk {
    /// @brief The last stated `totalResults`.
    std::size_t total{};
    /// @brief Entitlements received.
    std::size_t received{};
    /// @brief Pages requested.
    int requests{};
    /// @brief Whether it stopped at the page limit.
    bool truncated{};
  };

  /// @brief The account's entitlements.
  static constexpr std::string_view entitlements_url =
      "https://m.np.playstation.com/api/entitlement/v2/users/me/internal/"
      "entitlements";
  /// @brief The fields the concepts walk asks for.
  static constexpr std::string_view wanted_fields = "conceptMeta,titleMeta";
  /// @brief The fields the PS5 games walk asks for.
  static constexpr std::string_view games_fields =
      "titleMeta,gameMeta,productMeta,conceptMeta,streamingSupported,skuMeta,"
      "preorderFlag,preorderCoreFlag,isPlayTimeFullyConsumed";
  /// @brief The fields the single PS5 game asks for.
  static constexpr std::string_view game_fields =
      "gameMeta,titleMeta,productMeta,conceptMeta,streamingSupported,skuMeta,"
      "preorderCoreFlag,isPlayTimeFullyConsumed";

  /// @brief The concepts page request, logged with its query.
  static constexpr net::http::Endpoint concepts_endpoint{
      .label = label,
      .log_query = true,
  };
  /// @brief The PS5 games page request, logged with its query.
  static constexpr net::http::Endpoint games_endpoint{
      .label = games_label,
      .log_query = true,
  };
  /// @brief The single PS5 game request.
  static constexpr net::http::Endpoint game_endpoint{.label = game_label};

  /// @brief Reads pages until the stated total, an empty page, the page
  /// limit, or `take` declining a page.
  ///
  /// @tparam Take Callable as `take (Reply&)` returning whether to keep the
  ///     page and continue.
  /// @param asked Which list.
  /// @param package_type The package type.
  /// @param take Keeps a page's entitlements.
  /// @param observe Told each kept page; may be empty.
  /// @return What the walk saw.
  /// @retval Error The failures concepts_of() lists.
  template <class Take>
  [[nodiscard]] auto walk(Asked asked, std::string_view package_type,
                          Take&& take, const LibraryPageObserver& observe)
      -> Expected<Walk, WalkError>;
  /// @brief Walks the PS5 games once, appending them.
  ///
  /// @param all Where the games and counts go.
  /// @param observe Told each page; may be empty.
  /// @return True when the revision held for the whole walk; false when it
  ///     changed and the walk stopped.
  /// @retval Error The failures of walk().
  [[nodiscard]] auto games_pass(GameEntitlements& all,
                                const LibraryPageObserver& observe)
      -> Expected<bool, GamesPassError>;
  /// @brief Builds a page request.
  ///
  /// @param asked Which list.
  /// @param package_type The package type.
  /// @param offset Index of the first entitlement.
  /// @return The request.
  auto request(Asked asked, std::string_view package_type,
               std::size_t offset) const -> net::http::Request;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
