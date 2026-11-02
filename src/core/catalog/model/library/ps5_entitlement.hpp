/// @file
/// @brief Declares Ps5Entitlement, one record of the mobile internal
/// entitlement list that PS5 ownership is built from.
///
/// The streamable games are the active, unified PSGD full games and trials
/// whose streamingSupported is exactly true; used-up Play First Trials are
/// listed as ended trials. A further rule decides when a preordered game is
/// released. Both rules live here so that the scan and the launch re-read
/// judge a record the same way.
#pragma once

#include "src/core/catalog/model/library/exact_id.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/release_date.hpp>
#include <libchiaking/detail/catalog/title.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog {

/// @brief One PS5 entitlement the account holds, as the internal entitlement
/// list reported it.
///
/// Every field PS may omit is optional, so "absent" and "false" stay
/// distinct; an absent active or streaming flag never counts as a game. A
/// value is immutable, holds no references and may be shared between
/// threads freely.
class Ps5Entitlement {
 public:
  /// @brief featureType of a trial, which streams like a full game.
  static constexpr std::int64_t trial_feature = 1;
  /// @brief featureType of a full game.
  static constexpr std::int64_t full_game_feature = 3;
  /// @brief featureType of a game that needs its disc.
  static constexpr std::int64_t disc_required_feature = 5;
  /// @brief featureType of a Play First Trial.
  static constexpr std::int64_t play_first_trial_feature = 7;
  /// @brief entitlementType of a unified entitlement, the only kind
  /// accepted.
  static constexpr std::int64_t unified_entitlement = 5;
  /// @brief The gameMeta packageType of a PS5 game.
  static constexpr std::string_view game_package = "PSGD";

  /// @brief The fields read from one entitlement record.
  struct Facts {
    /// @brief The entitlement id; the identity of an owned PS5 tile.
    OwnershipId id;
    /// @brief The product it was bought under; absent or unparseable
    /// means the record is not a game.
    std::optional<ProductId> product{};
    /// @brief skuMeta's SKU id, when present and well formed.
    std::optional<SkuId> sku{};
    /// @brief titleMeta's title id, matched against the exclusion list.
    std::optional<TitleId> title{};
    /// @brief The concept the game belongs to; for grouping only.
    std::optional<ConceptId> concept_id{};
    /// @brief The game's display name; empty when PS sent none.
    std::string name{};
    /// @brief The first of the concept, game and product icon URLs,
    /// cut to the adapter's length limit; empty when none.
    std::string cover_url{};
    /// @brief isActive; std::nullopt when PS left it out.
    std::optional<bool> active{};
    /// @brief entitlementType; see unified_entitlement.
    std::optional<std::int64_t> entitlement_type{};
    /// @brief featureType; see the *_feature constants.
    std::optional<std::int64_t> feature_type{};
    /// @brief gameMeta's packageType; see game_package.
    std::string package_type{};
    /// @brief streamingSupported; std::nullopt when PS left it out.
    std::optional<bool> streaming_supported{};
    /// @brief isPlayTimeFullyConsumed; true stops a Play First Trial from
    /// streaming, as on a PS5. std::nullopt when PS left it out.
    std::optional<bool> play_time_used_up{};
    /// @brief preorderCoreFlag was exactly true; gates the release.
    bool preorder_core{};
    /// @brief preorderPlaceholderFlag was exactly true; gates the
    /// release. Whether PS sends this flag is unverified.
    bool preorder_placeholder{};
    /// @brief skuMeta's playabilityDate as text, when sent.
    std::optional<std::string> playable_from{};
    /// @brief activeDate as text, when sent.
    std::optional<std::string> active_date{};

    /// @brief Compares every field.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Keeps one record's fields.
  ///
  /// @param facts The fields as parsed from PS's reply.
  explicit Ps5Entitlement(Facts facts) noexcept;

  /// @brief The streamable games among some records, as owned titles.
  ///
  /// @param owned The records, in the order they should be kept.
  /// @return One title per distinct entitlement id that as_owned_title
  ///     accepts; the first record wins a repeated id. Empty when none.
  static auto titles_of(std::span<const Ps5Entitlement> owned)
      -> std::vector<OwnedTitle>;

  /// @brief The fields as read.
  ///
  /// @return The fields, valid while this record lives.
  auto facts() const noexcept -> const Facts& { return facts_; }
  /// @brief The entitlement id.
  ///
  /// @return The id, valid while this record lives.
  auto id() const noexcept -> const OwnershipId& { return facts_.id; }
  /// @brief Why this record is not a streamable game, if it is not.
  ///
  /// Checked in order: no product; not exactly active; a feature type other
  /// than a full game, trial or Play First Trial (disc required, or other);
  /// not a unified entitlement; a package other than PSGD; streaming
  /// support absent or false; a Play First Trial whose play time is used
  /// up. Names are never consulted.
  ///
  /// @return The first reason that applies, or std::nullopt for a game.
  auto why_not_a_game() const -> std::optional<NotAGame>;
  /// @brief When the game may be streamed.
  ///
  /// Only a record with preorderCoreFlag or preorderPlaceholderFlag is
  /// gated; its release time is playabilityDate, else activeDate.
  ///
  /// @return ReleaseDate::unrestricted for an ungated record,
  ///     ReleaseDate::unannounced when the gated record has no date, it
  ///     does not parse, or it is not after the epoch; otherwise the date.
  auto release() const -> ReleaseDate;
  /// @brief Whether the name looks like a demo, trial or digital extra.
  ///
  /// Diagnostics only: the scan logs such games but keeps them, because on
  /// PS5 PS's record decides what is a game.
  ///
  /// @return True when the name carries such a mark.
  auto is_named_like_an_extra() const -> bool;
  /// @brief The record as an owned PS5 title, when it is a game.
  ///
  /// The title is on PS5 cloud streaming and streams. A trial is held as
  /// HeldAs::trial_access, every other game as HeldAs::full_game.
  ///
  /// @return The title, or std::nullopt when why_not_a_game names a reason.
  [[nodiscard]] auto as_owned_title() const -> std::optional<OwnedTitle>;
  /// @brief The record as an ended trial, when it is a Play First Trial
  /// whose play time is used up and passes every other check.
  ///
  /// The title is held as HeldAs::trial_access with
  /// OwnedTitle::trial_ended set, so the catalog lists it without clearing
  /// it for launch.
  ///
  /// @return The title, or std::nullopt unless why_not_a_game names exactly
  ///     NotAGame::play_first_trial.
  [[nodiscard]] auto as_ended_trial() const -> std::optional<OwnedTitle>;

  /// @brief Compares every field.
  friend auto operator==(const Ps5Entitlement&, const Ps5Entitlement&)
      -> bool = default;

 private:
  /// @brief The record as an owned PS5 title, whatever why_not_a_game says.
  ///
  /// @param trial_ended Whether the title is an ended trial.
  /// @return The title, or std::nullopt when OwnedTitle::from refuses it.
  auto title(bool trial_ended) const -> std::optional<OwnedTitle>;
  /// @brief The refusal the feature type gives.
  ///
  /// @return std::nullopt for a full game, a trial or a Play First Trial;
  ///     NotAGame::disc_required for that type; NotAGame::other_feature for
  ///     any other or an absent type.
  auto feature_refusal() const noexcept -> std::optional<NotAGame>;
  /// @brief Whether the feature type is a trial or a Play First Trial,
  /// either of which is held as trial access.
  ///
  /// @return True for feature types 1 and 7.
  auto is_trial() const noexcept -> bool;
  /// @brief The refusal the streaming flag gives.
  ///
  /// @return NotAGame::streaming_unknown when the flag is absent,
  ///     NotAGame::streaming_not_supported when it is false, std::nullopt
  ///     when it is true.
  auto streaming_refusal() const noexcept -> std::optional<NotAGame>;
  /// @brief Whether a preorder flag holds the game back until its release.
  ///
  /// preorderFlag alone is not a trigger.
  ///
  /// @return True when preorderCoreFlag or preorderPlaceholderFlag is set.
  auto is_release_gated() const noexcept -> bool;

  /// @brief The fields as parsed; set once at construction.
  Facts facts_;
};

}  // namespace libchiaking::catalog
