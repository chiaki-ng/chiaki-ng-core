/// @file
/// @brief Declares MyGame, one title as it stands for one user, and the values
/// that describe that standing.
#pragma once

#include "libchiaking/detail/catalog/owned_title.hpp"
#include "libchiaking/detail/catalog/plus_inclusion.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/release_date.hpp"
#include "libchiaking/detail/catalog/storefront_answer.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/catalog/title.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::catalog {

/// @brief The lowest PlayStation Plus tier that may stream games from the
/// cloud.
inline constexpr users::Tier streaming_tier = users::Tier::premium;

/// @brief What gives the user the right to stream a title.
enum class PlayPath : std::uint8_t {
  /// @brief The title is offered by the older LegacyCloudStream service.
  old_service,
  /// @brief The user's plan includes the title.
  included,
  /// @brief The user owns the title and no plan includes it.
  owned,
  /// @brief The user does not own the title and no plan includes it; it has
  /// to be bought.
  store_only,
  /// @brief No plan includes the title and the library could not be read, so
  /// ownership is not known.
  unknown
};

/// @brief A trial version the store offers for a game the user does not hold.
enum class TrialOffer : std::uint8_t {
  /// @brief No trial version is offered, or the user already holds it.
  none,
  /// @brief An ordinary trial (featureType TRIAL), which the store calls a
  /// demo.
  demo,
  /// @brief A timed Play First Trial (featureType PLAY_FIRST_TRIAL). The
  /// user may already have used it up: PS deletes a used trial from the
  /// account while the store keeps offering it.
  play_first_trial,
};

/// @brief Whether the user owns a title.
enum class OwnershipState : std::uint8_t {
  /// @brief The user owns the title.
  owned,
  /// @brief The user does not own the title.
  not_owned,
  /// @brief The library could not be read.
  unknown,
};

/// @brief Why a listed game is kept out of the visible games.
enum class HideReason : std::uint8_t {
  /// @brief No plan covers the title on the service that would stream it.
  not_in_service,
  /// @brief The owned PS5 listing would not launch a PS5 title.
  not_ps5_native,
  /// @brief Another shown game already launches the same identifier.
  duplicate_launch,
  /// @brief The user owns another edition of the same game;
  /// Visibility::because_of names it.
  already_owned,
  /// @brief The user's tier is below streaming_tier, which hides every game.
  tier_does_not_include,
  /// @brief PS says the owned PS5 copy cannot be streamed and no plan
  /// includes the title.
  copy_does_not_stream,
};

/// @brief How a game stands for the user, as a library tile would label it.
enum class Standing : std::uint8_t {
  /// @brief PS's list marks the title as unstreamable; this takes
  /// precedence over every other standing.
  may_stream,
  /// @brief The user owns the game.
  in_library,
  /// @brief The user's plan includes the game.
  included,
  /// @brief The game has to be bought.
  store,
  /// @brief Ownership could not be read.
  ownership_unknown
};

/// @brief The identifier the streaming service is asked to start for a game.
///
/// It is either the listed product id or the id of the user's entitlement,
/// whichever copy the game launches from.
class LaunchIdentifier {
 public:
  /// @brief Longest identifier accepted, in bytes.
  static constexpr std::size_t limit = PrintableIdentifier::limit;

  /// @brief Makes the identifier that launches a listed product.
  ///
  /// @param product The listed product the game launches from.
  /// @return The identifier carrying the product id text.
  static auto of(const ProductId& product) -> LaunchIdentifier;
  /// @brief Makes the identifier that launches an owned copy.
  ///
  /// @param ownership The entitlement the game launches from.
  /// @return The identifier carrying the entitlement text.
  static auto of(const OwnershipId& ownership) -> LaunchIdentifier;

  /// @brief The identifier as it is sent.
  ///
  /// @return The identifier text; valid as long as this object.
  auto text() const noexcept -> const std::string& { return text_.text(); }
  /// @brief The title number inside the identifier, when it contains one.
  ///
  /// @return The title number, or nothing when the text contains none.
  auto title_number() const -> std::optional<TitleNumber>;
  /// @brief Whether the identifier names a PS5 title, that is, contains
  /// "PPSA".
  ///
  /// @return True when the text carries the PS5 platform marker; false
  /// otherwise.
  auto is_ps5_native() const noexcept -> bool;

  /// @brief Compares the identifiers byte for byte.
  friend auto operator==(const LaunchIdentifier&, const LaunchIdentifier&)
      -> bool = default;
  /// @brief Orders by text, so identifiers can key a map or set.
  friend auto operator<=>(const LaunchIdentifier&, const LaunchIdentifier&)
      -> std::strong_ordering = default;

 private:
  using Text = StrongText<LaunchIdentifier, PrintableIdentifier>;

  explicit LaunchIdentifier(std::string_view text)
      : text_(*Text::parse(text)) {}

  Text text_;
};

/// @brief Whether a listed game is shown, and if not, why and because of which
/// other product.
class Visibility {
 public:
  /// @brief Builds the visibility of a game that is shown.
  ///
  /// @return A visibility with no hide reason and no causing product.
  static auto visible() noexcept -> Visibility;
  /// @brief Builds the visibility of a game that is hidden.
  ///
  /// @param reason Why the game is hidden.
  /// @param because_of The other product that causes it, for reasons that
  ///     have one.
  /// @return The visibility.
  static auto hidden(HideReason reason,
                     std::optional<ProductId> because_of = std::nullopt)
      -> Visibility;

  /// @brief Whether the game is shown.
  ///
  /// @return True when there is no hide reason; false when the game is hidden.
  auto is_visible() const noexcept -> bool { return !reason_.has_value(); }
  /// @brief Why the game is hidden; empty when it is shown.
  ///
  /// @return The reason, or nothing when the game is shown.
  auto hide_reason() const noexcept -> std::optional<HideReason> {
    return reason_;
  }
  /// @brief The product that causes the hiding: the owned edition for
  /// HideReason::already_owned, the game that launches first for
  /// HideReason::duplicate_launch; otherwise empty.
  ///
  /// @return The causing product, or nothing when the reason has none or the
  /// game
  ///     is shown; valid as long as this object.
  auto because_of() const noexcept -> const std::optional<ProductId>& {
    return because_of_;
  }

  /// @brief Compares reason and causing product.
  friend auto operator==(const Visibility&, const Visibility&)
      -> bool = default;

 private:
  Visibility(std::optional<HideReason> reason,
             std::optional<ProductId> because_of) noexcept;

  std::optional<HideReason> reason_;
  std::optional<ProductId> because_of_;
};

/// @brief One game inside a PS5 Premium bundle, launched by its own ids.
///
/// A Premium category product whose SKUs carry more than one distinct
/// entitlement that is a streamable full game is a bundle. Its tile lists one
/// BundledGame per such entitlement. The bundle itself is never launched: the
/// user chooses one of its games, which launches by its own entitlement, with
/// the bundle's product asked of the selected-title resolver. Plain data.
///
/// @see MyGame::children
struct BundledGame {
  /// @brief The child's own entitlement, as the category lists it.
  OwnershipId entitlement;
  /// @brief The entitlement read as a product id; empty when its text is not
  /// shaped like one.
  std::optional<ProductId> product{};
  /// @brief Whether the account holds this entitlement and it is active,
  /// when the tile was built.
  bool owned{};

  /// @brief Compares every field.
  friend auto operator==(const BundledGame&, const BundledGame&)
      -> bool = default;
};

/// @brief One title as it stands for one user: what it is, how it may be
/// played, whether it is owned and whether it is shown.
///
/// A MyGame is immutable and consistent from construction; MyGame::from refuses
/// combinations of facts that contradict each other.
///
/// @see MyGames
/// @see LaunchVerdict::of
class MyGame {
 public:
  /// @brief Why a set of facts does not describe a possible game.
  enum class Invalid : std::uint8_t {
    /// @brief The path is PlayPath::owned but no owned copy is given.
    owned_path_without_ownership,
    /// @brief The path is PlayPath::store_only but an owned copy is given.
    store_only_path_with_ownership,
    /// @brief The remembered refusal names a product the title is not known
    /// as.
    refusal_for_another_title,
    /// @brief The path is PlayPath::store_only but ownership was never
    /// checked.
    store_only_path_without_ownership_check,
    /// @brief The path is PlayPath::unknown although ownership was checked
    /// or a copy is given.
    unknown_path_with_known_ownership,
  };

  /// @brief The other generation's version of the same game, listed on the
  /// other streaming service.
  ///
  /// PS's store sells the PS4 and PS5 versions of a cross-generation game
  /// together; LegacyCloudStream streams the PS4 version and PS5 cloud the PS5
  /// one.
  struct OtherVersion {
    /// @brief The product the other version is listed under.
    ProductId product;
    /// @brief Whether this version leads the pair, so a library shows it
    /// and offers the other version inside it.
    bool leads{};

    /// @brief Compares both fields.
    friend auto operator==(const OtherVersion&, const OtherVersion&)
        -> bool = default;
  };

  /// @brief The facts a MyGame is built from.
  struct Facts {
    /// @brief The title as PS lists it.
    Title title;
    /// @brief Whether a plan includes the title, and from which tier.
    PlusInclusion inclusion;
    /// @brief What gives the user the right to stream it.
    PlayPath path{};
    /// @brief Whether the game is shown.
    Visibility visibility;
    /// @brief What the streaming service is asked to start.
    LaunchIdentifier launch_identifier;
    /// @brief The user's copy, when the user owns the game.
    std::optional<OwnedTitle> ownership{};
    /// @brief A refusal by PS's streaming service that still stands for
    /// this title.
    std::optional<StreamRefusal> refusal{};
    /// @brief Whether the library was read, so that a missing copy means
    /// the game is not owned rather than unknown.
    bool ownership_checked{true};
    /// @brief The Premium category's offer to add the title to the library;
    /// set only for a PS5 Premium title the user does not own.
    std::optional<StorefrontAnswer::LibraryOffer> library_offer{};
    /// @brief The games inside a PS5 Premium bundle; empty when the title
    /// is not a bundle.
    std::vector<BundledGame> children{};
    /// @brief Whether one of the bundle's SKUs has every full-game
    /// entitlement owned and active, which decides between offering Stream
    /// and Purchase; a SKU naming no entitlement counts as owned.
    /// Only meaningful with children. It never gives the bundle an owned
    /// copy or a launchable identity.
    bool bundle_owned{};
    /// @brief The trial version the store offers for a PS5 title that is
    /// only for sale; TrialOffer::none for any other title.
    TrialOffer trial_offer{TrialOffer::none};
    /// @brief The other generation's version listed on the other service;
    /// std::nullopt when the store links none or it is not listed.
    std::optional<OtherVersion> other_version{};

    /// @brief Compares every fact.
    friend auto operator==(const Facts&, const Facts&) -> bool = default;
  };

  /// @brief Builds a game from facts, refusing facts that contradict each
  /// other.
  ///
  /// @param facts The facts about the game.
  /// @return The game, or the first contradiction found; see MyGame::Invalid
  ///     for each.
  [[nodiscard]] static auto from(Facts facts) -> Expected<MyGame, Invalid>;

  /// @brief The product id the game is listed under.
  ///
  /// @return The product id of the title; valid as long as this object.
  auto product_id() const noexcept -> const ProductId& {
    return facts_.title.product_id();
  }
  /// @brief The title as PS lists it.
  ///
  /// @return The title; valid as long as this object.
  auto title() const noexcept -> const Title& { return facts_.title; }
  /// @brief Whether a plan includes the title, and from which tier.
  ///
  /// @return The plan inclusion; valid as long as this object.
  auto inclusion() const noexcept -> const PlusInclusion& {
    return facts_.inclusion;
  }
  /// @brief What gives the user the right to stream it.
  ///
  /// @return The path that gives the right to stream.
  auto play_path() const noexcept -> PlayPath { return facts_.path; }
  /// @brief Whether the game is shown, and why not.
  ///
  /// @return The visibility; valid as long as this object.
  auto visibility() const noexcept -> const Visibility& {
    return facts_.visibility;
  }
  /// @brief What the streaming service is asked to start.
  ///
  /// @return The launch identifier; valid as long as this object.
  auto launch_identifier() const noexcept -> const LaunchIdentifier& {
    return facts_.launch_identifier;
  }
  /// @brief The user's copy; empty when the game is not owned or ownership is
  /// unknown.
  ///
  /// @return The owned copy, or nothing when the game is not owned or ownership
  ///     is unknown; valid as long as this object.
  auto ownership() const noexcept -> const std::optional<OwnedTitle>& {
    return facts_.ownership;
  }
  /// @brief A refusal by PS's streaming service that still stands for this
  /// title.
  ///
  /// @return The standing refusal, or nothing when none is remembered; valid as
  ///     long as this object.
  auto remembered_refusal() const noexcept
      -> const std::optional<StreamRefusal>& {
    return facts_.refusal;
  }

  /// @brief The offer to add an unowned PS5 Premium title to the library.
  ///
  /// The library never takes the offer itself. It names the SKU and the
  /// included entitlement from the Premium category, and is what a launch
  /// refusal hands the frontend so the user can add the title.
  ///
  /// @return The offer, or nothing when the title is owned or is not a PS5
  ///     Premium title; valid as long as this object.
  auto library_offer() const noexcept
      -> const std::optional<StorefrontAnswer::LibraryOffer>& {
    return facts_.library_offer;
  }
  /// @brief The games inside this title when it is a PS5 Premium bundle.
  ///
  /// @return One entry per streamable full game in the bundle, in category
  ///     order; empty when the title is not a bundle. Valid as long as this
  ///     object.
  auto children() const noexcept -> std::span<const BundledGame> {
    return facts_.children;
  }
  /// @brief Finds the bundled game a launch or ownership question names.
  ///
  /// @param product The id asked about; matches a child by its product id or
  ///     by its entitlement id written as a product id.
  /// @return The first matching child, or nothing when no child matches or
  ///     the title is not a bundle.
  auto child_known_as(const ProductId& product) const
      -> std::optional<BundledGame>;
  /// @brief Whether this title is a PS5 Premium bundle, which is launched
  /// only by choosing one of its children.
  ///
  /// @return True when the title lists bundled games; false otherwise.
  auto is_bundle() const noexcept -> bool { return !facts_.children.empty(); }
  /// @brief Whether the user owns the bundle, so that a frontend shows it as
  /// owned and offers to choose a game to play.
  ///
  /// Owning the bundle makes no child owned: each child is launched only
  /// when its own entitlement is owned, and is otherwise refused with the
  /// add-to-library offer. MyGame::is_owned stays false for a bundle.
  ///
  /// @return True for a bundle one of whose SKUs is wholly owned; false for
  ///     any other bundle and for a title that is not a bundle.
  auto is_bundle_owned() const noexcept -> bool {
    return is_bundle() && facts_.bundle_owned;
  }
  /// @brief The trial version the store offers instead of the full game.
  ///
  /// Set only for a PS5 title that is for sale, when one of its SKUs grants
  /// a streamable trial version whose id the user does not hold active; a
  /// Play First Trial is preferred over a demo. PS deletes a used Play
  /// First Trial from the account while the store category keeps listing
  /// it, so the offer may name a trial the user has already used. It is a
  /// label only: the trial is taken in the PlayStation Store.
  ///
  /// @return The offered trial, or TrialOffer::none.
  auto trial_offer() const noexcept -> TrialOffer { return facts_.trial_offer; }
  /// @brief The other generation's version of the game, listed on the other
  /// streaming service.
  ///
  /// @return The other version and whether this one leads the pair, or
  ///     nothing when the store links no listed version; valid as long as
  ///     this object.
  auto other_version() const noexcept -> const std::optional<OtherVersion>& {
    return facts_.other_version;
  }

  /// @brief Whether the user owns a copy.
  ///
  /// @return True when an owned copy is present; false otherwise.
  auto is_owned() const noexcept -> bool {
    return facts_.ownership.has_value();
  }
  /// @brief Whether the game is owned, not owned, or unknown because the
  /// library was not read.
  ///
  /// @return OwnershipState::owned when a copy is present;
  ///     OwnershipState::not_owned when none is and the library was read;
  ///     OwnershipState::unknown otherwise.
  auto ownership_state() const noexcept -> OwnershipState;
  /// @brief Whether this is a PS5 cloud title whose ownership is unknown, so
  /// it must be asked before launch.
  ///
  /// @see Catalog::recheck_ownership
  /// @return True when the title is on PS5 cloud streaming and ownership is
  ///     unknown; false otherwise.
  auto needs_ownership_check() const noexcept -> bool;
  /// @brief The ownership situation in the form a StreamRefusal records.
  ///
  /// @return The holding derived from the owned copy and whether the library
  /// was
  ///     read.
  auto holding() const -> StreamRefusal::Holding;
  /// @brief Whether the game is shown.
  ///
  /// @return True when the game is shown; false when it is hidden.
  auto is_visible() const noexcept -> bool {
    return facts_.visibility.is_visible();
  }
  /// @brief Whether the user owns the game and it is a PS5 title.
  ///
  /// @return True when a copy is owned and the title is a PS5 title; false
  ///     otherwise.
  auto is_in_ps5_library() const noexcept -> bool;
  /// @brief Whether the plan, rather than ownership, gives the right to
  /// stream.
  ///
  /// @return True when the play path is the old service or the plan inclusion;
  ///     false otherwise.
  auto plays_through_subscription() const noexcept -> bool;
  /// @brief How the game stands for the user, as a library tile would label
  /// it.
  ///
  /// @return The standing, with PS's unstreamable mark taking precedence over
  ///     the other standings.
  auto standing() const noexcept -> Standing;
  /// @brief The release date of the owned copy; unrestricted when the game is
  /// not owned.
  ///
  /// @return The owned copy's release date, or an unrestricted date when no
  /// copy
  ///     is owned.
  auto release() const noexcept -> ReleaseDate;
  /// @brief Whether this is a PS5 cloud title the user is known not to own,
  /// which must be in the library before it streams.
  ///
  /// @return True when the title is on PS5 cloud streaming and the library was
  /// read
  ///     without finding a copy; false otherwise.
  auto needs_library_add() const noexcept -> bool;
  /// @brief Whether this is a PS5 cloud title the plan includes and the user
  /// does not own yet.
  ///
  /// @return True when the title is on PS5 cloud streaming, no copy is owned
  /// and
  ///     the plan gives the right to stream; false otherwise.
  auto can_be_added_to_library() const noexcept -> bool;
  /// @brief Whether this is a PS5 title on PS5 cloud streaming, the kind the
  /// storefront is asked about before launch.
  ///
  /// @return True when the title is a PS5 title on PS5 cloud streaming; false
  ///     otherwise.
  auto needs_storefront_check() const noexcept -> bool;

  /// @brief Returns a copy with a different visibility.
  ///
  /// @param visibility The visibility the copy takes.
  /// @return The amended game; this object is unchanged.
  auto with_visibility(Visibility visibility) const -> MyGame;
  /// @brief Returns a copy linked to the other generation's version, or to
  /// none.
  ///
  /// @param other The other version, or nothing to drop the link.
  /// @return The amended game; this object is unchanged.
  auto with_other_version(std::optional<OtherVersion> other) const -> MyGame;
  /// @brief Returns a copy whose title no longer answers to one alias.
  ///
  /// A remembered refusal that named the alias is dropped with it.
  ///
  /// @param alias The alias to remove.
  /// @return The amended game.
  auto without_alias(const ProductId& alias) const -> MyGame;
  /// @brief Returns a copy with a different remembered refusal, or none.
  ///
  /// @param refusal The refusal that stands, or nothing to clear it.
  /// @return The amended game.
  /// @retval MyGame::Invalid::refusal_for_another_title The refusal names a
  ///     product the title is not known as.
  auto with_refusal(std::optional<StreamRefusal> refusal) const
      -> Expected<MyGame, Invalid>;

  /// @brief Compares every fact.
  friend auto operator==(const MyGame&, const MyGame&) -> bool = default;

 private:
  explicit MyGame(Facts facts) noexcept;

  Facts facts_;
};

}  // namespace libchiaking::catalog
