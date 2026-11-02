/// @file
/// @brief Declares cloud_stream::CloudGameListing, the selected-title
/// resolver's answer for one product at launch.
///
/// The launch awaits GET /api/catalog/v2/products/cloudgame for the row's
/// product with the user's plan ids. The answer is either the product PS
/// lists, a reply that lists nothing, a failed request, or a question that was
/// never asked because the user has no plan ids or no account locale.
#pragma once

#include "src/core/catalog/model/library/exact_id.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief What the cloudgame resolver said about one product.
///
/// Exactly one of listed, not listed, failed or not asked. A value is
/// immutable and may be shared between threads.
class CloudGameListing {
 public:
  /// @brief Why the resolver was not asked.
  enum class Unasked : std::uint8_t {
    /// @brief The user's cloud streaming benefits carry no plan ids.
    no_plan_ids,
    /// @brief The account has no legal country or no language.
    no_account_locale,
  };

  /// @brief One title the resolver lists under the product.
  struct Title {
    /// @brief The title id; std::nullopt when absent or malformed.
    std::optional<TitleId> title_id{};
    /// @brief The entitlement that launches the title; std::nullopt when
    /// absent or malformed.
    std::optional<OwnershipId> entitlement{};
    /// @brief The platform the title was made for, as PS wrote it.
    std::string native_platform{};
    /// @brief PS's isDownloadable, when sent.
    std::optional<bool> downloadable{};
    /// @brief PS's gameIntent, when sent.
    std::optional<std::string> game_intent{};

    /// @brief Compares every field.
    friend auto operator==(const Title&, const Title&) -> bool = default;
  };

  /// @brief The first product the resolver lists.
  struct Product {
    /// @brief The listed product id; std::nullopt when malformed.
    std::optional<ProductId> id{};
    /// @brief The titles under it; std::nullopt when the reply had no
    /// titles array, empty when the array was empty.
    std::optional<std::vector<Title>> titles{};

    /// @brief Compares both fields.
    friend auto operator==(const Product&, const Product&) -> bool = default;
  };

  /// @brief The resolver listed the product.
  ///
  /// @param product The first product it listed.
  /// @param http_status The reply's status.
  /// @return The answer.
  static auto listed(Product product, std::uint16_t http_status)
      -> CloudGameListing;
  /// @brief The resolver answered but listed no product.
  ///
  /// @param http_status The reply's status.
  /// @return The answer.
  static auto not_listed(std::uint16_t http_status) -> CloudGameListing;
  /// @brief The request failed.
  ///
  /// @param cause Why; may be a cancellation.
  /// @return The answer.
  static auto failed(Unanswered cause) -> CloudGameListing;
  /// @brief The resolver was not asked.
  ///
  /// @param why Why not.
  /// @return The answer.
  static auto not_asked(Unasked why) -> CloudGameListing;

  /// @brief Whether the resolver listed the product.
  ///
  /// @return True for a listed answer.
  auto is_listed() const noexcept -> bool;
  /// @brief Whether the resolver answered without listing it.
  ///
  /// @return True for a not-listed answer.
  auto is_not_listed() const noexcept -> bool;
  /// @brief The listed product.
  ///
  /// @return The product, or std::nullopt unless listed.
  auto product() const -> std::optional<Product>;
  /// @brief Why the request failed.
  ///
  /// @return The cause, or std::nullopt unless failed.
  auto cause() const -> std::optional<Unanswered>;
  /// @brief Why the resolver was not asked.
  ///
  /// @return The reason, or std::nullopt unless not asked.
  auto unasked() const noexcept -> std::optional<Unasked>;
  /// @brief The HTTP status of the reply.
  ///
  /// @return The status of a listed or not-listed reply, the failure's
  ///     status when it carries one, std::nullopt when not asked.
  auto http_status() const noexcept -> std::optional<std::uint16_t>;
  /// @brief The resolver's answer as a failure the launch reports.
  ///
  /// @return The cause of a failed request,
  ///     Unanswered::Reason::unavailable when it was not asked, and
  ///     std::nullopt when PS answered, listed or not.
  [[nodiscard]] auto unanswered() const -> std::optional<Unanswered>;

 private:
  /// @brief A listed answer.
  struct Listed {
    /// @brief The first product listed.
    Product product;
    /// @brief The reply's status.
    std::uint16_t http_status;
  };
  /// @brief An answer listing nothing.
  struct NotListed {
    /// @brief The reply's status.
    std::uint16_t http_status;
  };
  /// @brief A failed request.
  struct Failed {
    /// @brief Why it failed.
    Unanswered cause;
  };
  /// @brief A question never asked.
  struct NotAsked {
    /// @brief Why not.
    Unasked why;
  };

  /// @brief The four possible answers.
  using Answer = std::variant<Listed, NotListed, Failed, NotAsked>;

  /// @brief Keeps one answer.
  ///
  /// @param answer The answer.
  explicit CloudGameListing(Answer answer) noexcept;

  /// @brief The answer; set once at construction.
  Answer answer_;
};

}  // namespace libchiaking::catalog::cloud_stream
