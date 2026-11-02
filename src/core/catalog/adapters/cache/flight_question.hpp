/// @file
/// @brief Declares cache::FlightQuestion, the text that names a question for a
/// shared flight.
#pragma once

#include <libchiaking/detail/catalog/catalog.hpp>
#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/users/account_id.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog::cache {

/// @brief The text that names a question for a shared flight.
///
/// Two callers share a request exactly when their questions have the same
/// text. Immutable.
class FlightQuestion {
 public:
  /// @brief A question about an account.
  ///
  /// @param account The account.
  /// @return "account/{id}".
  static auto by(AccountId account) -> FlightQuestion;
  /// @brief A question about an account and a product.
  ///
  /// @param account The account.
  /// @param product The product.
  /// @return "account/{id}/{product}".
  static auto by(AccountId account, const ProductId& product) -> FlightQuestion;
  /// @brief A question about a region's streamable list.
  ///
  /// @param region The region.
  /// @return "region/{locale}", lower-case.
  static auto listed_in(const StoreLocale& region) -> FlightQuestion;
  /// @brief A question about the Premium category.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @return "{locale}/age-{age}", lower-case.
  static auto included_in(const StoreLocale& locale, int age) -> FlightQuestion;
  /// @brief A question about the for-sale category.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @return "for-sale/{locale}/age-{age}", lower-case.
  static auto for_sale_in(const StoreLocale& locale, int age) -> FlightQuestion;
  /// @brief A question about a product's details.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @param product The product.
  /// @return "details/{locale}/{age}/product/{product}".
  static auto sold_as(const StoreLocale& locale, int age,
                      const ProductId& product) -> FlightQuestion;
  /// @brief A question about a concept's details.
  ///
  /// @param locale The locale.
  /// @param grouped_under The concept.
  /// @return "details/{locale}/concept/{concept}".
  static auto grouped_as(const StoreLocale& locale,
                         const ConceptId& grouped_under) -> FlightQuestion;
  /// @brief A question about an image.
  ///
  /// @param image The image.
  /// @return The image's URL.
  static auto showing(const ImageRef& image) -> FlightQuestion;

  /// @brief The question's text.
  ///
  /// @return The text, valid while the question lives.
  auto text() const noexcept -> std::string_view { return text_; }

 private:
  /// @brief Keeps a question's text.
  ///
  /// @param text The text.
  explicit FlightQuestion(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief The question's text.
  std::string text_;
};

}  // namespace libchiaking::catalog::cache
