/// @file
/// @brief Declares cache::HeldBuilds, each account's latest MyGames build and
/// the answers it was made from.
#pragma once

#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>
#include <libchiaking/detail/users/account_id.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace libchiaking::catalog::cache {

/// @brief Every account's latest MyGames build, kept so an unchanged build is
/// not made again.
///
/// Not synchronised: its owner holds one lock around every call, so that a
/// change here and a change to the owner's other parts happen together.
class HeldBuilds {
 public:
  /// @brief A held MyGames build.
  using GamesPtr = std::shared_ptr<const MyGames>;

  /// @brief The answers a MyGames build was made from, compared by identity to
  /// tell whether the held build is still current.
  struct MatchedFrom {
    /// @brief The streamable list.
    std::shared_ptr<const StreamableList> list{};
    /// @brief The library.
    std::shared_ptr<const Library> titles{};
    /// @brief The PS5 scan.
    std::shared_ptr<const OwnedPs5Games> ps5{};
    /// @brief The Premium category.
    std::shared_ptr<const cloud_stream::PremiumCategory> category{};
    /// @brief The cloud streaming access the build saw.
    std::optional<users::CloudStreaming> streaming{};
    /// @brief The for-sale category.
    std::shared_ptr<const cloud_stream::ForSaleCategory> for_sale{};
  };

  /// @brief The held build, when it is still current.
  ///
  /// @param account The account.
  /// @param sources The answers a new build would use.
  /// @param inputs The inputs a new build would use.
  /// @return The held build when it was made from the same answers, with alike
  ///     streaming access, and from the same inputs; nullptr otherwise.
  auto matched_before(AccountId account, const MatchedFrom& sources,
                      const MyGames::Inputs& inputs) const -> GamesPtr;
  /// @brief Holds a build and the answers it was made from.
  ///
  /// @param account The account.
  /// @param games The build.
  /// @param sources What it was made from.
  void keep(AccountId account, const GamesPtr& games, MatchedFrom sources);
  /// @brief Forgets the account's build.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief A build and what it was made from.
  struct Build {
    /// @brief The build.
    GamesPtr games{};
    /// @brief What it was made from.
    MatchedFrom matched{};
  };

  /// @brief Whether two cloud streaming accesses gate tiles alike.
  ///
  /// @param held The access a held build saw.
  /// @param asked The access now.
  /// @return True when both allow or both do not, and both hide tiles or both
  ///     do not.
  static auto gates_alike(
      const std::optional<users::CloudStreaming>& held,
      const std::optional<users::CloudStreaming>& asked) noexcept -> bool;
  /// @brief Whether two builds were made from the same answers.
  ///
  /// @param held What the held build was made from.
  /// @param asked What a new build would use.
  /// @return True when every answer is the same object and the access gates
  ///     alike.
  static auto same_sources(const MatchedFrom& held,
                           const MatchedFrom& asked) noexcept -> bool;

  /// @brief The builds, by account id.
  std::map<std::uint64_t, Build> builds_;
};

}  // namespace libchiaking::catalog::cache
