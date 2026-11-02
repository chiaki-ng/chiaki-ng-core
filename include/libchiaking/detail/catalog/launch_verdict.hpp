/// @file
/// @brief Declares LaunchVerdict, the answer to whether a title may be streamed
/// now, with LaunchRefusal and ClearedLaunch.
#pragma once

#include "libchiaking/detail/catalog/call_failure.hpp"
#include "libchiaking/detail/catalog/my_game.hpp"
#include "libchiaking/detail/catalog/owned_title.hpp"
#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/release_date.hpp"
#include "libchiaking/detail/catalog/store_locale.hpp"
#include "libchiaking/detail/catalog/storefront_answer.hpp"
#include "libchiaking/detail/catalog/streamable_list.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace libchiaking::catalog {

/// @brief Why a title may not be streamed right now.
///
/// A refusal holds exactly one reason, with the details that belong to that
/// reason only; the accessors for the other kinds return empty values. A
/// frontend maps the kind to a message and an action, such as opening the store
/// or asking the user to sign in. The value is immutable and safe to share
/// between threads.
///
/// @see LaunchVerdict
class LaunchRefusal {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief The reason for the refusal.
  enum class Kind : std::uint8_t {
    /// @brief The product is not among the user's games, or no games are
    /// held yet.
    not_listed,
    /// @brief The game is listed but hidden; LaunchRefusal::hide_reason
    /// says why.
    hidden,
    /// @brief The user does not own the title and no plan includes it; it
    /// must be bought.
    buy_to_stream,
    /// @brief A PS5 cloud title the plan includes is not in the user's
    /// library yet and has to be added first; LaunchRefusal::library_offer
    /// may carry the store's offer.
    add_to_library,
    /// @brief The user's tier is below streaming_tier;
    /// LaunchRefusal::tier_held gives the tier held.
    needs_premium,
    /// @brief PS's streaming service refused this title recently and
    /// nothing has changed since; LaunchRefusal::time says when.
    refused_by_service,
    /// @brief PS says the product does not stream, as when PlayStation's
    /// restriction list excludes it; LaunchRefusal::notice carries the
    /// text to show.
    not_streamable,
    /// @brief The game has no announced release date yet.
    unannounced,
    /// @brief The game is released at LaunchRefusal::time, which is still
    /// ahead.
    not_released,
    /// @brief The game has a release date but no trusted time is available
    /// to compare it with.
    no_trusted_clock,
    /// @brief A check needed for the verdict could not be completed;
    /// LaunchRefusal::cause says why.
    check_unavailable,
    /// @brief Re-reading the owned entitlement found no record of it, or
    /// a record naming no product; error code ERR_INVALID_TITLE.
    invalid_title,
    /// @brief The owned entitlement is inactive and not marked as
    /// streaming; error code ERR_TITLE_TAKEDOWN.
    title_taken_down,
    /// @brief The owned entitlement is active but not marked as
    /// streaming; error code ERR_STREAMING_UNAVAILABLE.
    streaming_unavailable,
    /// @brief The owned entitlement is marked as streaming but is
    /// inactive, so it must be bought again; error code ERR_PURCHASE_NEEDED.
    purchase_needed,
    /// @brief The owned entitlement is active and marked as streaming but
    /// is still not a streamable full game, such as a trial or a
    /// non-game package; error code
    /// ERR_UNEXPECTED_ENTITLEMENT_STATE.
    unexpected_entitlement_state,
    /// @brief The product is a PS5 Premium bundle, which is never launched
    /// itself; the user chooses one of MyGame::children and that game is
    /// launched by its own product or entitlement.
    choose_a_game,
  };

  /// @brief The product is not among the user's games.
  struct NotListed {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const NotListed&, const NotListed&)
        -> bool = default;
  };
  /// @brief The game is listed but hidden.
  struct Hidden {
    /// @brief Why the game is hidden.
    HideReason reason{};
    /// @brief Compares the reason.
    friend auto operator==(const Hidden&, const Hidden&) -> bool = default;
  };
  /// @brief The title must be bought before it can stream.
  struct BuyToStream {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const BuyToStream&, const BuyToStream&)
        -> bool = default;
  };
  /// @brief The title must be added to the library first.
  struct AddToLibrary {
    /// @brief The store's add-to-library offer, when the store gave one.
    std::optional<StorefrontAnswer::LibraryOffer> offer{};
    /// @brief Compares the offer.
    friend auto operator==(const AddToLibrary&, const AddToLibrary&)
        -> bool = default;
  };
  /// @brief The user's tier is below streaming_tier.
  struct NeedsPremium {
    /// @brief The tier held, when known.
    std::optional<users::Tier> held{};
    /// @brief Compares the tier.
    friend auto operator==(const NeedsPremium&, const NeedsPremium&)
        -> bool = default;
  };
  /// @brief PS's streaming service refused the title recently.
  struct RefusedByService {
    /// @brief When it refused.
    Time when{};
    /// @brief Compares the time.
    friend auto operator==(const RefusedByService&, const RefusedByService&)
        -> bool = default;
  };
  /// @brief PS says the copy or product does not stream.
  struct NotStreamable {
    /// @brief The text to show; may be empty.
    std::string notice{};
    /// @brief Compares the text.
    friend auto operator==(const NotStreamable&, const NotStreamable&)
        -> bool = default;
  };
  /// @brief The game has no announced release date.
  struct Unannounced {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const Unannounced&, const Unannounced&)
        -> bool = default;
  };
  /// @brief The game is released at a time still ahead.
  struct NotReleased {
    /// @brief When the game is released.
    Time at{};
    /// @brief Compares the time.
    friend auto operator==(const NotReleased&, const NotReleased&)
        -> bool = default;
  };
  /// @brief There is a release date but no trusted time to compare it with.
  struct NoTrustedClock {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const NoTrustedClock&, const NoTrustedClock&)
        -> bool = default;
  };
  /// @brief A check needed for the verdict could not be completed.
  struct CheckUnavailable {
    /// @brief Why the check could not be completed.
    LaunchCheckFailure cause;
  };
  /// @brief The owned entitlement has no record or no product.
  struct InvalidTitle {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const InvalidTitle&, const InvalidTitle&)
        -> bool = default;
  };
  /// @brief The owned entitlement is inactive and not streaming.
  struct TitleTakenDown {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const TitleTakenDown&, const TitleTakenDown&)
        -> bool = default;
  };
  /// @brief The owned entitlement is active and not streaming.
  struct StreamingUnavailable {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const StreamingUnavailable&,
                           const StreamingUnavailable&) -> bool = default;
  };
  /// @brief The owned entitlement is streaming but inactive.
  struct PurchaseNeeded {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const PurchaseNeeded&, const PurchaseNeeded&)
        -> bool = default;
  };
  /// @brief The owned entitlement is active and streaming but not a
  /// streamable full game.
  struct UnexpectedEntitlementState {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const UnexpectedEntitlementState&,
                           const UnexpectedEntitlementState&) -> bool = default;
  };
  /// @brief A bundle was asked for; one of its games has to be chosen.
  struct ChooseAGame {
    /// @brief Compares nothing; every value is equal.
    friend auto operator==(const ChooseAGame&, const ChooseAGame&)
        -> bool = default;
  };

  /// @brief The reason with the details that belong to it, one alternative
  /// per LaunchRefusal::Kind and in the same order.
  using Reason =
      std::variant<NotListed, Hidden, BuyToStream, AddToLibrary, NeedsPremium,
                   RefusedByService, NotStreamable, Unannounced, NotReleased,
                   NoTrustedClock, CheckUnavailable, InvalidTitle,
                   TitleTakenDown, StreamingUnavailable, PurchaseNeeded,
                   UnexpectedEntitlementState, ChooseAGame>;

  /// @brief Builds a refusal from its reason.
  ///
  /// @param reason Why the title may not launch, with its details.
  explicit LaunchRefusal(Reason reason) noexcept : reason_(std::move(reason)) {}

  /// @brief Builds a LaunchRefusal::Kind::not_listed refusal.
  ///
  /// @return The refusal.
  static auto not_listed() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::hidden refusal.
  ///
  /// @param reason Why the game is hidden.
  /// @return The refusal.
  static auto hidden(HideReason reason) -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::buy_to_stream refusal.
  ///
  /// @return The refusal.
  static auto buy_to_stream() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::add_to_library refusal.
  ///
  /// @param offer The store's add-to-library offer, when the store was asked
  ///     and gave one.
  /// @return The refusal.
  static auto add_to_library(
      std::optional<StorefrontAnswer::LibraryOffer> offer = std::nullopt)
      -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::needs_premium refusal.
  ///
  /// @param held The tier the user holds, when known.
  /// @return The refusal.
  static auto needs_premium(std::optional<users::Tier> held) -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::refused_by_service refusal.
  ///
  /// @param when When the streaming service refused.
  /// @return The refusal.
  static auto refused_by_service(Time when) -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::not_streamable refusal.
  ///
  /// @param notice The text explaining why; may be empty.
  /// @return The refusal.
  static auto not_streamable(std::string notice) -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::unannounced refusal.
  ///
  /// @return The refusal.
  static auto unannounced() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::not_released refusal.
  ///
  /// @param at When the game is released.
  /// @return The refusal.
  static auto not_released(Time at) -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::no_trusted_clock refusal.
  ///
  /// @return The refusal.
  static auto no_trusted_clock() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::check_unavailable refusal.
  ///
  /// @param cause Why the check could not be completed.
  /// @return The refusal.
  static auto check_unavailable(LaunchCheckFailure cause) -> LaunchRefusal;
  /// @brief Builds the refusal for a PS5 cloud title whose ownership could
  /// not be read.
  ///
  /// @return A LaunchRefusal::Kind::check_unavailable refusal whose cause is
  ///     LaunchCheckReason::ownership_unknown.
  /// @see LaunchRefusal::asks_for_ownership_check
  static auto ownership_unknown() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::invalid_title refusal.
  ///
  /// @return The refusal.
  static auto invalid_title() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::title_taken_down refusal.
  ///
  /// @return The refusal.
  static auto title_taken_down() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::streaming_unavailable refusal.
  ///
  /// @return The refusal.
  static auto streaming_unavailable() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::purchase_needed refusal.
  ///
  /// @return The refusal.
  static auto purchase_needed() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::unexpected_entitlement_state
  /// refusal.
  ///
  /// @return The refusal.
  static auto unexpected_entitlement_state() -> LaunchRefusal;
  /// @brief Builds a LaunchRefusal::Kind::choose_a_game refusal.
  ///
  /// @return The refusal.
  static auto choose_a_game() -> LaunchRefusal;
  /// @brief Turns a release date into the refusal it calls for, if any.
  ///
  /// @param release The release date of the owned copy.
  /// @param trusted_now The trusted current time, when one is available.
  /// @return Nothing when the game is released; otherwise an unannounced,
  ///     not_released or no_trusted_clock refusal.
  [[nodiscard]] static auto of_release(ReleaseDate release,
                                       std::optional<Time> trusted_now)
      -> std::optional<LaunchRefusal>;

  /// @brief The reason for the refusal, for switching on.
  ///
  /// @return The kind matching the alternative reason() holds.
  auto kind() const noexcept -> Kind {
    return static_cast<Kind>(reason_.index());
  }
  /// @brief The reason with its details, for std::visit.
  ///
  /// @return A reference into this refusal, valid while it lives.
  auto reason() const noexcept -> const Reason& { return reason_; }
  /// @brief Why the game is hidden.
  ///
  /// @return The reason for LaunchRefusal::Kind::hidden; nothing for any
  ///     other kind.
  auto hide_reason() const noexcept -> std::optional<HideReason>;
  /// @brief The store's add-to-library offer.
  ///
  /// @return The offer for LaunchRefusal::Kind::add_to_library when the store
  ///     gave one; nothing otherwise.
  auto library_offer() const -> std::optional<StorefrontAnswer::LibraryOffer>;
  /// @brief The tier the user holds.
  ///
  /// @return The tier for LaunchRefusal::Kind::needs_premium when known;
  ///     nothing otherwise.
  auto tier_held() const noexcept -> std::optional<users::Tier>;
  /// @brief When PS refused, or when the game is released.
  ///
  /// @return The time for refused_by_service and not_released; nothing for
  ///     every other kind.
  auto time() const noexcept -> std::optional<Time>;
  /// @brief The text to show for LaunchRefusal::Kind::not_streamable.
  ///
  /// The text is a fixed English sentence, such as the one shown when
  /// PlayStation's restriction list excludes the title.
  ///
  /// @return A view into this refusal, empty for every other kind.
  auto notice() const noexcept -> std::string_view;
  /// @brief Why a check could not be completed.
  ///
  /// @return The cause for LaunchRefusal::Kind::check_unavailable; nothing
  ///     otherwise.
  auto cause() const -> std::optional<LaunchCheckFailure>;
  /// @brief Whether the refusal means ownership is unknown and should be
  /// asked again.
  ///
  /// @return True for a check_unavailable refusal whose cause is
  ///     LaunchCheckReason::ownership_unknown.
  /// @see Catalog::recheck_ownership
  auto asks_for_ownership_check() const noexcept -> bool;

 private:
  Reason reason_;
};
/// @brief What to hand to the streaming service once a launch has been allowed.
///
/// @see LaunchVerdict::cleared_launch
struct ClearedLaunch {
  /// @brief The game's listed product id.
  ProductId product;
  /// @brief Which streaming service carries the game.
  ServiceGeneration service{};
  /// @brief The console generation the game was made for.
  Platform platform{};
  /// @brief The identifier the streaming service is asked to start.
  LaunchIdentifier identifier;
  /// @brief The user's entitlement for the game; empty when the game is
  /// played through the plan without being owned.
  std::optional<OwnershipId> held_as{};
  /// @brief The store locale the streamable list was answered in.
  StoreLocale store;
  /// @brief Whether the list came from the account's service or the public
  /// store.
  ListOrigin origin{};
  /// @brief Whether the copy launched is held as trial access, a PS5 trial
  /// or Play First Trial, so a refusal later in the launch is logged in
  /// full.
  bool trial{};

  /// @brief Compares every field.
  friend auto operator==(const ClearedLaunch&, const ClearedLaunch&)
      -> bool = default;
};

/// @brief Whether a title may be streamed now: allowed, optionally with what to
/// launch, or refused with a reason.
///
/// @see Catalog::can_launch
/// @see Catalog::clear_for_launch
class LaunchVerdict {
 public:
  /// @brief Builds an allowed verdict that carries no ClearedLaunch.
  ///
  /// @return The verdict, for which LaunchVerdict::is_allowed is true and
  ///     LaunchVerdict::refusal is null.
  static auto allowed() -> LaunchVerdict;
  /// @brief Builds an allowed verdict that says what to launch.
  ///
  /// @param launch What to hand to the streaming service.
  /// @return The verdict.
  static auto cleared(ClearedLaunch launch) -> LaunchVerdict;
  /// @brief Builds a refused verdict.
  ///
  /// @param refusal Why the title may not launch.
  /// @return The verdict.
  static auto refused(LaunchRefusal refusal) -> LaunchVerdict;
  /// @brief Judges a game from what the games list says about it, making no
  /// request.
  ///
  /// The rules are applied in this order and the first that applies decides:
  /// not listed, hidden, a bundle whose game must be chosen, ownership
  /// unknown, must be bought, must be added to the library, not released,
  /// recently refused by PS. A bundle's child is judged by
  /// Catalog::can_launch with the child's product, not here.
  ///
  /// @param listed The game, or null when the product is not among the user's
  ///     games.
  /// @param trusted_now The trusted current time; when empty, a game with a
  ///     release date is refused with LaunchRefusal::Kind::no_trusted_clock.
  /// @return An allowed verdict without a ClearedLaunch, or the refusal.
  static auto of(const MyGame* listed,
                 std::optional<LaunchRefusal::Time> trusted_now)
      -> LaunchVerdict;
  /// @brief Judges a game like LaunchVerdict::of, except that a missing
  /// trusted time does not refuse.
  ///
  /// Suited to deciding whether to offer a play control: the release date is
  /// checked properly when the launch is cleared.
  ///
  /// @param listed The game, or null when the product is not among the user's
  ///     games.
  /// @param held_now The trusted current time, when one is already at hand.
  /// @return An allowed verdict without a ClearedLaunch, or the refusal.
  static auto before_launch(const MyGame* listed,
                            std::optional<LaunchRefusal::Time> held_now)
      -> LaunchVerdict;

  /// @brief Whether the title may launch.
  ///
  /// @return True when the verdict holds no refusal; false when it holds one.
  auto is_allowed() const noexcept -> bool {
    return !std::holds_alternative<LaunchRefusal>(outcome_);
  }
  /// @brief Why the title may not launch.
  ///
  /// @return The refusal held inside this verdict, valid while it lives;
  ///     null exactly when the verdict is allowed.
  auto refusal() const noexcept -> const LaunchRefusal* {
    return std::get_if<LaunchRefusal>(&outcome_);
  }
  /// @brief What to launch.
  ///
  /// @return The launch held inside this verdict, valid while it lives, for
  ///     an allowed verdict built with LaunchVerdict::cleared; null
  ///     otherwise.
  auto cleared_launch() const noexcept -> const ClearedLaunch* {
    return std::get_if<ClearedLaunch>(&outcome_);
  }

 private:
  struct Allowed {};

  using Outcome = std::variant<Allowed, ClearedLaunch, LaunchRefusal>;

  explicit LaunchVerdict(Outcome outcome) noexcept;

  Outcome outcome_;
};

}  // namespace libchiaking::catalog
