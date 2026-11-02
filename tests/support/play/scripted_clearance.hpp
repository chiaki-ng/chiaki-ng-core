#pragma once

#include "src/core/play/adapters/cloud_launch/title_clearance.hpp"

#include <libchiaking/detail/catalog/launch_check.hpp>
#include <libchiaking/detail/catalog/launch_verdict.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/stream_refusal.hpp>

#include "tests/support/setup.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::play {

struct Title {
  static auto product_id(std::string_view id)
      -> libchiaking::catalog::ProductId {
    return Setup::value(libchiaking::catalog::ProductId::parse(id),
                        "the product id parses");
  }

  std::string_view product{"EP0109-PPSA07033_00-FAKE0030TITLE000"};
  libchiaking::catalog::ServiceGeneration service{
      libchiaking::catalog::ServiceGeneration::ps5_cloud};
  libchiaking::catalog::Platform platform{libchiaking::catalog::Platform::ps5};
  std::string_view identifier{};
  std::string_view held_as{};
  std::string_view store{"en-US"};
  libchiaking::catalog::ListOrigin origin{
      libchiaking::catalog::ListOrigin::account_service};
  bool trial{};

  auto cleared() const -> libchiaking::catalog::ClearedLaunch {
    namespace catalog = libchiaking::catalog;
    auto launched_as = Setup::value(
        catalog::OwnershipId::parse(identifier.empty() ? product : identifier),
        "the launched title parses");
    auto locale =
        Setup::value(catalog::StoreLocale::parse(store), "the store parses");
    auto launched = std::optional{catalog::LaunchIdentifier::of(launched_as)};
    std::optional<catalog::OwnershipId> held;
    if (!held_as.empty())
      held = Setup::value(catalog::OwnershipId::parse(held_as),
                          "the held title parses");
    return catalog::ClearedLaunch{
        product_id(product), service,           platform, std::move(*launched),
        std::move(held),     std::move(locale), origin,   trial};
  }
};

class ScriptedClearance final : public libchiaking::play::TitleClearance {
 public:
  struct Remembered {
    libchiaking::AccountId account;
    libchiaking::catalog::ProductId product;
    libchiaking::catalog::StreamRefusal::Kind kind;
  };

  explicit ScriptedClearance(const Title& title)
      : verdict_(
            libchiaking::catalog::LaunchVerdict::cleared(title.cleared())) {}

  explicit ScriptedClearance(libchiaking::catalog::LaunchRefusal refusal)
      : verdict_(
            libchiaking::catalog::LaunchVerdict::refused(std::move(refusal))) {}

  auto held(libchiaking::users::PsUser&,
            const libchiaking::catalog::ProductId&) const
      -> libchiaking::catalog::LaunchVerdict override {
    return verdict_;
  }

  auto clear(libchiaking::users::PsUser&,
             const libchiaking::catalog::ProductId& asked,
             const libchiaking::platform::StopToken&,
             const libchiaking::catalog::LaunchCheckObserver& observer)
      -> libchiaking::catalog::LaunchVerdict override {
    ++clears;
    cleared_products.push_back(asked);
    for (const auto check : told) observer(check);
    if (refusal_at_clear)
      if (auto refusal = refusal_at_clear())
        return libchiaking::catalog::LaunchVerdict::refused(
            std::move(*refusal));
    return verdict_;
  }

  void remember_refusal(
      libchiaking::users::PsUser& user,
      const libchiaking::catalog::ProductId& refused,
      libchiaking::catalog::StreamRefusal::Kind kind) override {
    remembered.push_back({user.account_id(), refused, kind});
  }

  std::function<std::optional<libchiaking::catalog::LaunchRefusal>()>
      refusal_at_clear;
  std::vector<libchiaking::catalog::LaunchCheck> told;
  int clears{};
  std::vector<libchiaking::catalog::ProductId> cleared_products;
  std::vector<Remembered> remembered;

 private:
  libchiaking::catalog::LaunchVerdict verdict_;
};

class SwitchableClearance final : public libchiaking::play::TitleClearance {
 public:
  explicit SwitchableClearance(libchiaking::play::TitleClearance& answering)
      : answering_(&answering) {}

  void answer_with(libchiaking::play::TitleClearance& answering) noexcept {
    answering_ = &answering;
  }

  auto held(libchiaking::users::PsUser& user,
            const libchiaking::catalog::ProductId& product) const
      -> libchiaking::catalog::LaunchVerdict override {
    return answering_->held(user, product);
  }

  auto clear(libchiaking::users::PsUser& user,
             const libchiaking::catalog::ProductId& product,
             const libchiaking::platform::StopToken& stop,
             const libchiaking::catalog::LaunchCheckObserver& observer)
      -> libchiaking::catalog::LaunchVerdict override {
    return answering_->clear(user, product, stop, observer);
  }

  void remember_refusal(
      libchiaking::users::PsUser& user,
      const libchiaking::catalog::ProductId& product,
      libchiaking::catalog::StreamRefusal::Kind kind) override {
    answering_->remember_refusal(user, product, kind);
  }

 private:
  libchiaking::play::TitleClearance* answering_;
};

}  // namespace test_support::play
