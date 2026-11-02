/// @file
/// @brief Declares OwnedTitle, one game an account owns, and the identifiers
/// and classifications of owned records.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/release_date.hpp"
#include "libchiaking/detail/catalog/title.hpp"
#include "libchiaking/detail/catalog/title_number.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/strong_text.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog {

/// @brief The rule PS's copy and launch identifiers follow: 1 to
/// PrintableIdentifier::limit printable ASCII characters with no spaces.
///
/// OwnershipId and LaunchIdentifier both admit text through it.
struct PrintableIdentifier {
  /// @brief Longest identifier accepted, in bytes.
  static constexpr std::size_t limit = 128;

  /// @brief Whether a text follows the rule.
  ///
  /// @param text The candidate identifier.
  /// @return True when it is 1 to limit bytes, each a printable ASCII
  ///     character other than space. Never blocks.
  static auto admits(std::string_view text) noexcept -> bool;
};

/// @brief The identifier of one entitlement, that is, one copy of something the
/// account owns.
class OwnershipId {
 public:
  /// @brief Longest identifier accepted, in bytes.
  static constexpr std::size_t limit = PrintableIdentifier::limit;

  /// @brief Checks an identifier and wraps it.
  ///
  /// @param text The identifier as PS gave it.
  /// @return The identifier, or nothing unless the text is 1 to
  ///     OwnershipId::limit printable ASCII characters with no spaces.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<OwnershipId>;

  /// @brief The identifier as PS gave it.
  ///
  /// @return The identifier text; valid as long as this object.
  auto text() const noexcept -> const std::string& { return text_.text(); }
  /// @brief The title number inside the identifier, when it contains one.
  ///
  /// @return The title number, or nothing when the text contains none.
  auto title_number() const -> std::optional<TitleNumber>;
  /// @brief The key that groups identifiers of the same title; see TitleKey.
  ///
  /// @return The key, or nothing when the text yields none.
  auto title_key() const -> std::optional<TitleKey>;
  /// @brief The identifier read as a product id, when it has the form of one.
  ///
  /// @return The product id, or nothing when the text is not shaped like one.
  auto as_product() const -> std::optional<ProductId>;

  /// @brief Compares the identifiers byte for byte.
  friend auto operator==(const OwnershipId&, const OwnershipId&)
      -> bool = default;
  /// @brief Orders by text, so identifiers can key a map or set.
  friend auto operator<=>(const OwnershipId&, const OwnershipId&)
      -> std::strong_ordering = default;
  /// @brief Compares the identifier with plain text.
  friend auto operator==(const OwnershipId& id, std::string_view text) -> bool {
    return id.text_ == text;
  }

 private:
  using Text = StrongText<OwnershipId, PrintableIdentifier>;

  explicit OwnershipId(Text text) noexcept : text_(std::move(text)) {}

  Text text_;
};

/// @brief What kind of entitlement the account holds for a game.
enum class HeldAs : std::uint8_t {
  /// @brief The full game.
  full_game,
  /// @brief Trial access to the game; a full copy of the same product
  /// supersedes it.
  trial_access,
  /// @brief A disc upgrade entitlement; it is resolved to the game it
  /// upgrades.
  disc_upgrade,
  /// @brief Any other entitlement that still counts as a game.
  other_game
};

/// @brief Why an owned record is not counted as a game.
enum class NotAGame : std::uint8_t {
  /// @brief The record carries no game details.
  no_game_details,
  /// @brief The entitlement is not active.
  inactive,
  /// @brief The record is an add-on.
  add_on,
  /// @brief The record's package type marks a trial.
  trial_package,
  /// @brief The record's SKU type marks a trial.
  trial_sku,
  /// @brief The name marks a demo.
  demo_name,
  /// @brief The name marks a trial.
  trial_name,
  /// @brief A PS5 record whose package type marks a media application.
  media_app,
  /// @brief A PS5 record whose package type marks a tracking placeholder.
  tracking_placeholder,
  /// @brief A PS5 record whose name marks a digital extra rather than the
  /// game.
  digital_extra,
  /// @brief A PS5 record that names no product id it can be launched by.
  no_product,
  /// @brief A PS5 record whose feature type is a trial (1, `TRIAL`); no
  /// longer produced, since an active trial streams, and kept so stored
  /// explanations still read.
  trial,
  /// @brief A PS5 record whose feature type requires the disc (5,
  /// `DISC_REQUIRED`), which cannot stream.
  disc_required,
  /// @brief A PS5 Play First Trial (7, `PLAY_FIRST_TRIAL`) whose play time
  /// PS reports used up (`isPlayTimeFullyConsumed`), which a PS5 does
  /// not stream; one with time left streams as trial access. A used-up
  /// trial that passes every other check is still listed, as an ended
  /// trial that refuses to launch.
  play_first_trial,
  /// @brief A PS5 record whose feature type is anything other than a full
  /// game (3, `FULL_GAME`) or the trial and disc types above.
  other_feature,
  /// @brief A PS5 record whose entitlement type is not the unified
  /// entitlement type (5).
  not_unified,
  /// @brief A PS5 record whose package type is not a game (`PSGD`).
  other_package,
  /// @brief A PS5 record that PS marks as not supporting streaming.
  streaming_not_supported,
  /// @brief A PS5 record that does not say whether it supports streaming;
  /// only an explicit `true` is accepted.
  streaming_unknown,
};

/// @brief One game an account owns, reduced to what the catalog needs.
///
/// An owned title is immutable. Names that mark a demo or a trial are refused,
/// so an OwnedTitle is always a game.
///
/// @see Library
class OwnedTitle {
 public:
  /// @brief Why the facts do not describe an owned game.
  enum class Invalid : std::uint8_t {
    /// @brief The name marks a demo.
    demo_name,
    /// @brief The name marks a trial.
    trial_name,
  };

  /// @brief The facts an OwnedTitle is built from.
  struct Facts {
    /// @brief The entitlement; also what the streaming service starts for
    /// this copy.
    OwnershipId id;
    /// @brief The game's name as the record gives it.
    std::string name{};
    /// @brief The console generation the copy was made for.
    Platform platform{};
    /// @brief Which streaming service would carry the copy.
    ServiceGeneration service{};
    /// @brief What kind of entitlement it is.
    HeldAs held{HeldAs::other_game};
    /// @brief When the copy may be played.
    ReleaseDate release{};
    /// @brief The store product the copy was bought under, when the record
    /// names one.
    std::optional<ProductId> product{};
    /// @brief The concept the game belongs to, when the record names one.
    std::optional<ConceptId> concept_id{};
    /// @brief Address of the cover image; empty when the record has none.
    std::string cover_url{};
    /// @brief Whether PS says this copy can be streamed, from a PS5
    /// record's streaming flag or a PS4 or PS3 streaming package; empty
    /// when PS has not said.
    std::optional<bool> streams{};
    /// @brief Whether the copy is a PS5 trial that has ended: a Play First
    /// Trial whose play time PS reports used up, or a trial whose launch
    /// PS refused as ended (StreamRefusal::Kind::trial_ended). Listed so
    /// the player sees it and never cleared for launch from this fact.
    bool trial_ended{};
    /// @brief Whether the copy is a PS5 Play First Trial (featureType 7),
    /// a timed trial whose play time can be used up, rather than an
    /// ordinary trial (featureType 1) with no reported time limit.
    bool play_first_trial{};

    /// @brief Compares every fact.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Builds an owned title from facts.
  ///
  /// @param facts The facts about the copy.
  /// @return The title.
  /// @retval OwnedTitle::Invalid::demo_name The name marks a demo.
  /// @retval OwnedTitle::Invalid::trial_name The name marks a trial.
  [[nodiscard]] static auto from(Facts facts) -> Expected<OwnedTitle, Invalid>;

  /// @brief The entitlement; also what the streaming service starts for this
  /// copy.
  ///
  /// @return The entitlement id; valid as long as this object.
  auto id() const noexcept -> const OwnershipId& { return facts_.id; }
  /// @brief The game's name as the record gives it.
  ///
  /// @return The name; valid as long as this object.
  auto name() const noexcept -> const std::string& { return facts_.name; }
  /// @brief The console generation the copy was made for.
  ///
  /// @return The platform of the copy.
  auto platform() const noexcept -> Platform { return facts_.platform; }
  /// @brief Which streaming service would carry the copy.
  ///
  /// @return The streaming service generation.
  auto service() const noexcept -> ServiceGeneration { return facts_.service; }
  /// @brief What kind of entitlement it is.
  ///
  /// @return The kind of entitlement.
  auto held_as() const noexcept -> HeldAs { return facts_.held; }
  /// @brief When the copy may be played.
  ///
  /// @return The release date of the copy.
  auto release() const noexcept -> ReleaseDate { return facts_.release; }
  /// @brief The store product the copy was bought under, when the record
  /// names one.
  ///
  /// @return The product id, or nothing when the record names none; valid as
  /// long
  ///     as this object.
  auto product_id() const noexcept -> const std::optional<ProductId>& {
    return facts_.product;
  }
  /// @brief The concept the game belongs to, when the record names one.
  ///
  /// @return The concept id, or nothing when the record names none; valid as
  /// long
  ///     as this object.
  auto concept_id() const noexcept -> const std::optional<ConceptId>& {
    return facts_.concept_id;
  }
  /// @brief Address of the cover image; empty when the record has none.
  ///
  /// @return The cover address, or an empty string when the record has none;
  /// valid
  ///     as long as this object.
  auto cover_url() const noexcept -> const std::string& {
    return facts_.cover_url;
  }

  /// @brief Whether PS says this copy can be streamed, from a PS5 record's
  /// streaming flag or a PS4 or PS3 streaming package; empty when PS has
  /// not said.
  ///
  /// @return True or false as PS stated, or nothing when PS has not said.
  auto streams() const noexcept -> std::optional<bool> {
    return facts_.streams;
  }
  /// @brief Whether the copy is a PS5 trial that has ended, so it is listed
  /// but cannot be launched: PS reports its play time used up, or PS
  /// refused its launch as ended.
  ///
  /// @return True only for such a trial; false for every other copy.
  auto trial_ended() const noexcept -> bool { return facts_.trial_ended; }
  /// @brief Whether the copy is a PS5 Play First Trial, a timed trial whose
  /// play time can be used up, rather than an ordinary trial or demo.
  ///
  /// @return True only for a PS5 record whose featureType is 7.
  auto is_play_first_trial() const noexcept -> bool {
    return facts_.play_first_trial;
  }

  /// @brief The copy's title number, taken from the product id when it has
  /// one and from the entitlement otherwise.
  ///
  /// @return The number from the product id, else from the entitlement id, or
  ///     nothing when neither contains one.
  auto title_number() const -> std::optional<TitleNumber>;
  /// @brief Whether the copy answers to a product, by its product id or by
  /// its entitlement id.
  ///
  /// @param product The product id to test.
  /// @return True when the copy's product id equals the product or its
  ///     entitlement id has the same text; false otherwise.
  auto is_held_under(const ProductId& product) const noexcept -> bool;
  /// @brief Returns a copy whose release date is the earlier of its own and
  /// another.
  ///
  /// Used when several records describe the same copy: the copy is playable
  /// as soon as any of them is.
  ///
  /// @param other The other record's release date.
  /// @return The amended title.
  auto released_no_later_than(ReleaseDate other) const -> OwnedTitle;
  /// @brief Returns a copy marked as an ended trial, as when PS refused
  /// its launch with StreamRefusal::Kind::trial_ended.
  ///
  /// @return The title with OwnedTitle::trial_ended set; every other fact is
  ///     unchanged.
  auto as_ended_trial() const -> OwnedTitle;
  /// @brief Compares every fact.
  friend auto operator==(const OwnedTitle&, const OwnedTitle&)
      -> bool = default;

 private:
  explicit OwnedTitle(Facts facts) noexcept;

  Facts facts_;
};

}  // namespace libchiaking::catalog
