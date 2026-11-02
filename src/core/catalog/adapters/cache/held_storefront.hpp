/// @file
/// @brief Declares cache::HeldStorefront, the storefront answers held in
/// memory for each account.
#pragma once

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/storefront_answer.hpp>
#include <libchiaking/detail/users/account_id.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

namespace libchiaking::catalog::cache {

/// @brief Every account's storefront answers, by product.
///
/// Not synchronised: its owner holds one lock around every call, so that a
/// change here and a change to the owner's other parts happen together.
class HeldStorefront {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief A storefront answer that is still fresh.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @param now The wall-clock time now.
  /// @return The answer, or std::nullopt when none is held, it is for another
  ///     account, or it is no longer fresh.
  auto fresh(AccountId account, const ProductId& product, Time now) const
      -> std::optional<StorefrontAnswer>;
  /// @brief Holds a storefront answer.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @param answer The answer.
  void keep(AccountId account, const ProductId& product,
            const StorefrontAnswer& answer);
  /// @brief Drops a held storefront answer.
  ///
  /// @param account The account.
  /// @param product The product.
  void drop(AccountId account, const ProductId& product);
  /// @brief Forgets every answer held for an account.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief The answers, by account id and then product.
  std::map<std::uint64_t, std::map<ProductId, StorefrontAnswer>> answers_;
};

}  // namespace libchiaking::catalog::cache
