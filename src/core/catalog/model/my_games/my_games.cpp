#include "libchiaking/detail/catalog/my_games.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

MyGames::MyGames(AccountId account, std::vector<MyGame> games,
                 std::size_t visible, Inputs inputs) noexcept
    : account_(account),
      games_(std::move(games)),
      visible_(visible),
      inputs_(inputs) {}

auto MyGames::of(AccountId account, std::vector<MyGame> ordered, Inputs inputs)
    -> Expected<MyGames, Invalid> {
  for (auto game = ordered.begin(); game != ordered.end(); ++game) {
    const auto repeated = std::ranges::any_of(
        ordered.begin(), game, [&game](const MyGame& earlier) {
          return earlier.title().is_known_as(game->product_id());
        });
    if (repeated) return unexpected(Invalid::duplicate_product);
  }
  const auto misjudged = [&inputs](const MyGame& game) {
    const bool known = game.title().service() == ServiceGeneration::ps5_cloud
                           ? inputs.ps5_library == LibraryState::fetched
                           : inputs.library == LibraryState::fetched;
    const auto state = game.ownership_state();
    return known ? state == OwnershipState::unknown
                 : state == OwnershipState::not_owned;
  };
  if (std::ranges::any_of(ordered, misjudged))
    return unexpected(Invalid::ownership_knowledge);
  const auto hidden =
      std::ranges::stable_partition(ordered, &MyGame::is_visible);
  const auto visible =
      static_cast<std::size_t>(hidden.begin() - ordered.begin());
  return MyGames{account, std::move(ordered), visible, inputs};
}

auto MyGames::of(AccountId account, std::vector<MyGame> ordered, Inputs inputs,
                 std::vector<Explanation> explanations)
    -> Expected<MyGames, Invalid> {
  auto games = of(account, std::move(ordered), inputs);
  if (games) games.value().explanations_ = std::move(explanations);
  return games;
}

auto MyGames::visible() const noexcept -> std::span<const MyGame> {
  return std::span<const MyGame>{games_}.first(visible_);
}

auto MyGames::hidden() const noexcept -> std::span<const MyGame> {
  return std::span<const MyGame>{games_}.subspan(visible_);
}

auto MyGames::find(const ProductId& product) const noexcept -> const MyGame* {
  const auto found =
      std::ranges::find_if(games_, [&product](const MyGame& game) {
        return game.title().is_known_as(product);
      });
  return found != games_.end() ? &*found : nullptr;
}

auto MyGames::holding_of(const ProductId& product) const
    -> StreamRefusal::Holding {
  if (const auto* game = find(product)) return game->holding();
  const auto service = product.platform() == Platform::ps5
                           ? ServiceGeneration::ps5_cloud
                           : ServiceGeneration::old_service;
  return ownership_is_known_on(service) ? StreamRefusal::Holding::not_held()
                                        : StreamRefusal::Holding::unknown();
}

auto MyGames::explain(const ProductId& product) const -> Explanation {
  const auto* game = find(product);
  const auto& asked = game != nullptr ? game->product_id() : product;
  const auto told =
      std::ranges::find(explanations_, asked, &Explanation::product_id);
  if (told != explanations_.end()) return *told;
  return game != nullptr ? Explanation::of(asked, {})
                         : Explanation::not_listed(product);
}

}  // namespace libchiaking::catalog
