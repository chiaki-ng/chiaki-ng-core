/// @file
/// @brief Declares StorefrontAnswer, what the store said about one title when
/// asked as one account.
#pragma once

#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libchiaking::catalog {

/// @brief What the PlayStation Store said about one title for one account:
/// whether it streams, whether the account owns it and whether it can be added
/// to the library.
///
/// The answer is a snapshot that may be reused for StorefrontAnswer::lifetime.
///
/// @see Catalog::check_title_against_storefront
class StorefrontAnswer {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief What the store said about streaming the title.
  enum class Streaming : std::uint8_t {
    /// @brief The store says the title streams.
    streams,
    /// @brief The store says the title does not stream;
    /// StorefrontAnswer::notice carries its text.
    not_streamable,
    /// @brief The store answered for the title but said nothing about
    /// streaming.
    no_notice,
    /// @brief The store's reply held no product that matches the one asked
    /// about.
    edition_missing
  };

  /// @brief The store's offer to add a title to the library.
  struct LibraryOffer {
    /// @brief The SKU id the store attaches to the offer.
    std::string sku_id{};
    /// @brief The entitlement ids the store attaches to the offer.
    std::vector<std::string> entitlement_ids{};
    /// @brief Whether the store says the account is already entitled
    /// through its plan; empty when the store does not say.
    std::optional<bool> entitled_through_subscription{};

    /// @brief Compares every field.
    friend auto operator==(const LibraryOffer&, const LibraryOffer&)
        -> bool = default;
  };

  /// @brief One product in the store's reply, before the matching one is
  /// picked.
  struct Product {
    /// @brief The product the store answered for.
    ProductId id;
    /// @brief Whether the product streams; empty when the store says
    /// nothing about it.
    std::optional<bool> streaming_supported{};
    /// @brief The store's text about streaming the product.
    std::string notice{};
    /// @brief Whether the store says the account owns the product.
    bool owned{};
    /// @brief The store's add-to-library offer for the product, when it
    /// makes one.
    std::optional<LibraryOffer> library_offer{};
    /// @brief Whether the store offers the account a timed game trial of
    /// the product, such as a Play First Trial; the store stops offering
    /// one once it has been used. A demo does not count.
    bool offers_trial{};
  };

  /// @brief How long an answer may be reused before the store is asked again.
  static constexpr std::chrono::minutes lifetime{5};

  /// @brief Builds the answer by picking, from the store's reply, the product
  /// that was asked about.
  ///
  /// @param account The account the store was asked as.
  /// @param asked The product that was asked about.
  /// @param answered Every product in the store's reply; the first one that
  ///     ProductId::is_answered_by accepts is used.
  /// @param checked_at When the store answered.
  /// @return The answer. When no product matches, its streaming is
  ///     StorefrontAnswer::Streaming::edition_missing and nothing else is
  ///     set.
  static auto of(AccountId account, ProductId asked,
                 std::span<const Product> answered, Time checked_at)
      -> StorefrontAnswer;

  /// @brief The account the store was asked as.
  ///
  /// @return The account id the store was asked as.
  auto account() const noexcept -> AccountId { return account_; }
  /// @brief The product that was asked about.
  ///
  /// @return The asked product id, valid while this answer lives.
  auto product_id() const noexcept -> const ProductId& { return asked_; }
  /// @brief The product in the store's reply that matched; empty when none
  /// did.
  ///
  /// @return The matched product id, or nothing when no product in the reply
  ///     matched.
  auto answered_product() const noexcept -> const std::optional<ProductId>& {
    return answered_;
  }
  /// @brief What the store said about streaming the title.
  ///
  /// @return The streaming verdict;
  /// StorefrontAnswer::Streaming::edition_missing when
  ///     no product matched.
  auto streaming() const noexcept -> Streaming { return streaming_; }
  /// @brief The store's text for a title that does not stream; empty
  /// otherwise.
  ///
  /// @return The notice text, valid while this answer lives; empty when the
  /// store
  ///     gave none.
  auto notice() const noexcept -> const std::string& { return notice_; }
  /// @brief Whether the store says the account owns the product.
  ///
  /// This is the store's own view and can differ from the Library for a short
  /// time after a purchase.
  ///
  /// @return True when the store reported the product as owned; false
  /// otherwise.
  auto owned_per_store() const noexcept -> bool { return owned_; }
  /// @brief The store's add-to-library offer, when it makes one.
  ///
  /// @return The offer, or nothing when the store made none.
  auto library_offer() const noexcept -> const std::optional<LibraryOffer>& {
    return library_offer_;
  }
  /// @brief Whether the store offers the account a timed game trial of the
  /// product, such as a Play First Trial; a demo does not count.
  ///
  /// The store makes the offer for each account and drops it once the
  /// account has used the trial, unlike the public store category, which
  /// keeps listing it.
  ///
  /// @return True when the matched product carries a trial offer; false
  ///     otherwise, and when no product matched.
  auto offers_trial() const noexcept -> bool { return offers_trial_; }
  /// @brief When the store answered.
  ///
  /// @return The time given when the store answered.
  auto checked_at() const noexcept -> Time { return checked_at_; }

  /// @brief Whether the store says the title streams.
  ///
  /// @return True when the streaming verdict is
  /// StorefrontAnswer::Streaming::streams;
  ///     false otherwise.
  auto says_it_streams() const noexcept -> bool {
    return streaming_ == Streaming::streams;
  }
  /// @brief Whether the title can be added to the library now.
  ///
  /// @return True when the store makes an offer, says the account does not
  ///     own the product, and states that the account is not yet entitled
  ///     through its plan.
  auto offers_library_add() const noexcept -> bool;
  /// @brief Whether the answer is still recent enough to reuse.
  ///
  /// @param now The current wall-clock time.
  /// @return True from StorefrontAnswer::checked_at until
  ///     StorefrontAnswer::lifetime has passed; false for a time before the
  ///     check.
  auto is_fresh_at(Time now) const noexcept -> bool;

  /// @brief Compares every part of the answer.
  friend auto operator==(const StorefrontAnswer&, const StorefrontAnswer&)
      -> bool = default;

 private:
  StorefrontAnswer(AccountId account, ProductId asked,
                   Time checked_at) noexcept;

  AccountId account_;
  ProductId asked_;
  Time checked_at_;
  Streaming streaming_{Streaming::edition_missing};
  std::optional<ProductId> answered_{};
  std::string notice_{};
  bool owned_{};
  std::optional<LibraryOffer> library_offer_{};
  bool offers_trial_{};
};

}  // namespace libchiaking::catalog
