/// @file
/// @brief Declares OwnedRecord, one entitlement from the commerce
/// internal_entitlements list that LegacyCloudStream ownership is built from.
///
/// The commerce list mixes games with add-ons, trials, media apps and
/// placeholders, and its fields are loosely typed. OwnedRecord keeps the
/// fields the catalog reads and decides, by the heuristics LegacyCloudStream
/// needs, whether a record is a game and which of several records for one game
/// streams best. Its PS5 records are ignored for PS5 ownership; PS5
/// ownership comes from Ps5Entitlement.
#pragma once

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/release_date.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog {

/// @brief One commerce entitlement record, reduced to the fields the
/// LegacyCloudStream library needs.
///
/// A record is plain parsed data plus the rules that read it; it is
/// immutable after construction, holds no references, and may be copied and
/// shared between threads freely.
class OwnedRecord {
 public:
  /// @brief featureType of an add-on; never a game.
  static constexpr int add_on_feature = 0;
  /// @brief featureType of time-limited trial access.
  static constexpr int trial_access_feature = 1;
  /// @brief featureType of a full game.
  static constexpr int full_game_feature = 3;
  /// @brief featureType of a disc-to-digital upgrade.
  static constexpr int disc_upgrade_feature = 5;

  /// @brief The fields read from one record of PS's commerce reply.
  ///
  /// Strings are empty when PS left the field out.
  struct Fields {
    /// @brief The entitlement id; empty when the record named only a
    /// product.
    std::string id{};
    /// @brief The product the entitlement was bought under.
    std::string product_id{};
    /// @brief The game's name from gameMeta, or the record's own name
    /// when there is no gameMeta.
    std::string name{};
    /// @brief Whether the record carried a gameMeta block; records
    /// without one are not games.
    bool has_game_details{};
    /// @brief The record's activeFlag; false when absent.
    bool active{};
    /// @brief The numeric featureType (see the *_feature constants); 0
    /// when absent or not a number.
    int feature_type{};
    /// @brief gameMeta's packageType, such as PS4GD or PSGD; empty
    /// without gameMeta.
    std::string package_type{};
    /// @brief The record's skuType, or gameMeta's when the record has
    /// none.
    std::string sku_type{};
    /// @brief The first of "ps5", "ps4" or "ps3" the entitlement
    /// attributes name, lower-cased; empty when none does.
    std::string platform_id{};
    /// @brief activeDate as text, when PS sent one.
    std::optional<std::string> active_date{};
    /// @brief skuMeta's playabilityDate as text, when PS sent one.
    std::optional<std::string> playability_date{};
    /// @brief The record's preorderFlag; false when absent.
    bool preorder{};
    /// @brief The cover image URL, cut to the adapter's length limit;
    /// empty when the record has no image.
    std::string cover_url{};
    /// @brief The SKU id; used only to break ties between records.
    std::string sku_id{};
    /// @brief The concept the game belongs to; empty when PS named
    /// none or it did not parse.
    std::string concept_id{};

    /// @brief Compares every field.
    friend auto operator==(const Fields&, const Fields&) -> bool = default;
  };

  /// @brief Keeps one record's fields.
  ///
  /// @param fields The fields as parsed from PS's reply.
  explicit OwnedRecord(Fields fields) noexcept : fields_(std::move(fields)) {}

  /// @brief Builds the LegacyCloudStream library from every record of one read.
  ///
  /// Records are ranked by streams_better_than, so when several records
  /// name one OwnershipId the best streaming one is kept, with the earliest
  /// release date of them all. Records that are not games are dropped.
  /// Every record with both an id and a product adds its id to that
  /// product's bundle.
  ///
  /// @param account The account the records belong to.
  /// @param records Every record read; may be empty.
  /// @param fetched_at When the records were read.
  /// @return The library; it is never refused, since duplicates were
  ///     merged first.
  static auto library_of(AccountId account,
                         std::span<const OwnedRecord> records,
                         Library::Time fetched_at) -> Library;

  /// @brief The fields as read.
  ///
  /// @return The fields, valid while this record lives.
  auto fields() const noexcept -> const Fields& { return fields_; }
  /// @brief The product the entitlement was bought under.
  ///
  /// @return The product id, or std::nullopt when the record named none or
  ///     it did not parse.
  auto product_id() const -> std::optional<ProductId> {
    return ProductId::parse(fields_.product_id);
  }
  /// @brief When the game became or becomes playable.
  ///
  /// playabilityDate wins over activeDate. A preorder with neither date is
  /// unannounced; any other record with neither is unrestricted. A date that
  /// does not parse, or is not after the epoch, is unannounced.
  ///
  /// @return The release date.
  auto release() const -> ReleaseDate {
    return ReleaseDate::of_record(fields_.active_date, fields_.preorder,
                                  fields_.playability_date);
  }

  /// @brief How the account holds the game, from featureType.
  ///
  /// @return HeldAs::trial_access, HeldAs::full_game or
  ///     HeldAs::disc_upgrade for those feature types, HeldAs::other_game
  ///     for anything else.
  auto held_as() const noexcept -> HeldAs {
    switch (fields_.feature_type) {
      case trial_access_feature:
        return HeldAs::trial_access;
      case full_game_feature:
        return HeldAs::full_game;
      case disc_upgrade_feature:
        return HeldAs::disc_upgrade;
      default:
        break;
    }
    return HeldAs::other_game;
  }

  /// @brief Why this record is not counted as a game, if it is not.
  ///
  /// Checked in order: no gameMeta, inactive, add-on feature, a package
  /// type ending in "GT", a SKU type containing "trial", a demo or trial
  /// name. PS5-native records are then also refused as PSMEDIA, PSTRACK or
  /// a name marking a digital extra.
  ///
  /// @return The first reason that applies, or std::nullopt for a game.
  auto why_not_a_game() const -> std::optional<NotAGame>;
  /// @brief Whether the entitlement id (or the product id when there is no
  /// entitlement id) is a PS5 title number.
  ///
  /// @return True for a PPSA-style id; false otherwise.
  auto is_ps5_native() const noexcept -> bool;
  /// @brief Which streaming service the record belongs to.
  ///
  /// @return ServiceGeneration::old_service when the record is a PS4 or
  ///     PS3 game by its attributes or its id, otherwise
  ///     ServiceGeneration::ps5_cloud, including when no platform is known.
  auto service() const noexcept -> ServiceGeneration;
  /// @brief The record's platform, from its attributes, then its id.
  ///
  /// @return The platform; Platform::ps3 when neither names one.
  auto platform() const noexcept -> Platform;
  /// @brief How well this record serves as the one to stream, higher being
  /// better.
  ///
  /// Adds 4 when the entitlement id equals the product id, 2 for a full
  /// game feature or a package type ending in "GD", 1 for a non-empty
  /// entitlement id and 3 for a PS5 entitlement id.
  ///
  /// @return A score from 0 to 10.
  auto stream_rank() const noexcept -> int;
  /// @brief Whether this record should be preferred over another for the
  /// same game.
  ///
  /// Compares stream_rank, then prefers a package type ending in "GS", then
  /// the smaller SKU id, product id and entitlement id, so the order is
  /// total and stable.
  ///
  /// @param other The record to compare against.
  /// @return True when this record comes first.
  auto streams_better_than(const OwnedRecord& other) const noexcept -> bool;
  /// @brief The record as an owned title, when it is a game.
  ///
  /// A streaming package marks the title as streaming; any other package
  /// leaves that unstated.
  ///
  /// @return The title, or std::nullopt when why_not_a_game names a reason,
  ///     the launch id does not parse, or OwnedTitle refuses the name as a
  ///     demo or trial.
  [[nodiscard]] auto as_owned_title() const -> std::optional<OwnedTitle>;

  /// @brief Compares every field.
  friend auto operator==(const OwnedRecord&, const OwnedRecord&)
      -> bool = default;

 private:
  /// @brief The id a launch names: the entitlement id, or the product id
  /// when the record has none.
  ///
  /// @return A view into the fields, valid while this record lives;
  ///     empty when the record has neither.
  auto launch_id() const noexcept -> std::string_view;
  /// @brief The platform the record states: PS5 for a PS5-native id, else
  /// the platform its attributes name.
  ///
  /// @return The platform, or std::nullopt when neither says.
  auto stated_platform() const noexcept -> std::optional<Platform>;
  /// @brief The platform the title number inside the product id (or the
  /// entitlement id when there is no product) marks.
  ///
  /// @return The platform, or std::nullopt when the id holds no title
  ///     number.
  auto named_platform() const noexcept -> std::optional<Platform>;
  /// @brief Whether the package type ends in "GS", a streaming package.
  ///
  /// @return True for a streaming package.
  auto is_streaming_package() const noexcept -> bool;

  /// @brief The fields as parsed; set once at construction.
  Fields fields_;
};

}  // namespace libchiaking::catalog
