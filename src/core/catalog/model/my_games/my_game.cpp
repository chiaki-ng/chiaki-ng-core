#include "libchiaking/detail/catalog/my_game.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

auto LaunchIdentifier::of(const ProductId& product) -> LaunchIdentifier {
  return LaunchIdentifier{product.text()};
}

auto LaunchIdentifier::of(const OwnershipId& ownership) -> LaunchIdentifier {
  return LaunchIdentifier{ownership.text()};
}

auto LaunchIdentifier::title_number() const -> std::optional<TitleNumber> {
  return TitleNumber::from(text());
}

auto LaunchIdentifier::is_ps5_native() const noexcept -> bool {
  return TitleNumber::platform_marked_in(text()) == Platform::ps5;
}

Visibility::Visibility(std::optional<HideReason> reason,
                       std::optional<ProductId> because_of) noexcept
    : reason_(reason), because_of_(std::move(because_of)) {}

auto Visibility::visible() noexcept -> Visibility {
  return Visibility{std::nullopt, std::nullopt};
}

auto Visibility::hidden(HideReason reason, std::optional<ProductId> because_of)
    -> Visibility {
  return Visibility{reason, std::move(because_of)};
}

MyGame::MyGame(Facts facts) noexcept : facts_(std::move(facts)) {}

auto MyGame::from(Facts facts) -> Expected<MyGame, Invalid> {
  if (facts.path == PlayPath::owned && !facts.ownership)
    return unexpected(Invalid::owned_path_without_ownership);
  if (facts.path == PlayPath::store_only && facts.ownership)
    return unexpected(Invalid::store_only_path_with_ownership);
  if (facts.refusal && !facts.title.is_known_as(facts.refusal->product_id()))
    return unexpected(Invalid::refusal_for_another_title);
  if (facts.path == PlayPath::store_only && !facts.ownership_checked)
    return unexpected(Invalid::store_only_path_without_ownership_check);
  if (facts.path == PlayPath::unknown &&
      (facts.ownership || facts.ownership_checked))
    return unexpected(Invalid::unknown_path_with_known_ownership);
  return MyGame{std::move(facts)};
}

auto MyGame::ownership_state() const noexcept -> OwnershipState {
  if (is_owned()) return OwnershipState::owned;
  return facts_.ownership_checked ? OwnershipState::not_owned
                                  : OwnershipState::unknown;
}

auto MyGame::needs_ownership_check() const noexcept -> bool {
  return facts_.title.service() == ServiceGeneration::ps5_cloud &&
         ownership_state() == OwnershipState::unknown;
}

auto MyGame::holding() const -> StreamRefusal::Holding {
  return StreamRefusal::Holding::of(facts_.ownership, facts_.ownership_checked);
}

auto MyGame::child_known_as(const ProductId& product) const
    -> std::optional<BundledGame> {
  const auto found = std::ranges::find_if(
      facts_.children, [&product](const BundledGame& child) {
        return child.product == product ||
               child.entitlement == std::string_view{product.text()};
      });
  return found != facts_.children.end() ? std::optional{*found} : std::nullopt;
}

auto MyGame::is_in_ps5_library() const noexcept -> bool {
  return is_owned() && facts_.title.platform() == Platform::ps5;
}

auto MyGame::plays_through_subscription() const noexcept -> bool {
  return facts_.path == PlayPath::old_service ||
         facts_.path == PlayPath::included;
}

auto MyGame::standing() const noexcept -> Standing {
  if (facts_.title.streamability() == ListStreamability::marked_unstreamable)
    return Standing::may_stream;
  if (needs_ownership_check()) return Standing::ownership_unknown;
  if (is_in_ps5_library()) return Standing::in_library;
  switch (facts_.path) {
    case PlayPath::old_service:
    case PlayPath::included:
      return Standing::included;
    case PlayPath::owned:
      return Standing::in_library;
    case PlayPath::unknown:
      return Standing::ownership_unknown;
    case PlayPath::store_only:
      break;
  }
  return Standing::store;
}

auto MyGame::release() const noexcept -> ReleaseDate {
  return facts_.ownership ? facts_.ownership->release()
                          : ReleaseDate::unrestricted();
}

auto MyGame::needs_library_add() const noexcept -> bool {
  return facts_.title.service() == ServiceGeneration::ps5_cloud &&
         ownership_state() == OwnershipState::not_owned;
}

auto MyGame::can_be_added_to_library() const noexcept -> bool {
  return facts_.title.service() == ServiceGeneration::ps5_cloud &&
         !is_owned() && plays_through_subscription();
}

auto MyGame::needs_storefront_check() const noexcept -> bool {
  return facts_.title.service() == ServiceGeneration::ps5_cloud &&
         facts_.title.platform() == Platform::ps5;
}

auto MyGame::with_visibility(Visibility visibility) const -> MyGame {
  auto facts = facts_;
  facts.visibility = std::move(visibility);
  return MyGame{std::move(facts)};
}

auto MyGame::with_other_version(std::optional<OtherVersion> other) const
    -> MyGame {
  auto facts = facts_;
  facts.other_version = std::move(other);
  return MyGame{std::move(facts)};
}

auto MyGame::without_alias(const ProductId& alias) const -> MyGame {
  auto facts = facts_;
  facts.title = facts.title.without_alias(alias);
  if (facts.refusal && !facts.title.is_known_as(facts.refusal->product_id()))
    facts.refusal.reset();
  return MyGame{std::move(facts)};
}

auto MyGame::with_refusal(std::optional<StreamRefusal> refusal) const
    -> Expected<MyGame, Invalid> {
  auto facts = facts_;
  facts.refusal = std::move(refusal);
  return from(std::move(facts));
}

}  // namespace libchiaking::catalog
