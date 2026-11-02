/// @file
/// @brief Declares ps::StoreAppTitle, the adapter that asks the store,
/// as the signed-in user, what it offers for a title.
///
/// The answer (StorefrontAnswer) tells each edition's ownership, streaming
/// notice and add-to-library offer. The core reads the offer but never acts
/// on it.
#pragma once

#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/authenticated/title_details.hpp"
#include "src/core/psn/graphql/error.hpp"

#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/catalog/storefront_answer.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <array>
#include <optional>
#include <string_view>

namespace libchiaking::catalog::ps {

/// @brief Asks the store app about one title for one user.
///
/// Borrows the user, client and clock, which must outlive it.
class StoreAppTitle {
 public:
  /// @brief The store asked: empty, meaning the account's own store.
  static constexpr std::string_view account_store{};
  /// @brief The call-to-action type of an add-to-library offer.
  static constexpr std::string_view library_offer_action =
      "DOWNLOAD_PS_PLUS_GAME_CATALOG";
  /// @brief The call-to-action types that offer the account a timed
  /// subscription game trial. PS leaves them out once the account has used
  /// the trial. A demo's DOWNLOAD_TRIAL is not among them, since a demo is not
  /// used up.
  static constexpr std::array<std::string_view, 6> trial_actions{
      "DOWNLOAD_PS_PLUS_TRIAL",
      "UPSELL_PS_PLUS_TRIAL",
      "DOWNLOAD_PS_PLUS_FULL_GAME_TRIAL",
      "UPSELL_PS_PLUS_FULL_GAME_TRIAL",
      "DOWNLOAD_EA_ACCESS_PLAY_FIRST_TRIAL",
      "UPSELL_EA_ACCESS_PLAY_FIRST_TRIAL",
  };

  /// @brief Sets up the questions for a user.
  ///
  /// @param user The signed-in user; supplies the store app token. Must outlive
  ///     this object.
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the answer. Must outlive this object.
  StoreAppTitle(users::PsUser& user, net::http::Client& http,
                const platform::ClockProvider& wall) noexcept;

  /// @brief Asks about the concept of a product.
  ///
  /// Blocks on the store; when the store refuses the token it is renewed once
  /// and the question asked again.
  ///
  /// @param product The product; asked without its SKU suffix.
  /// @return The answer for the account; a cancellation;
  ///     users::Unanswered::Reason::store_access_lapsed when the token is
  ///     refused or missing; Unanswered::Reason::malformed,
  ///     Unanswered::Reason::not_listed, Unanswered::Reason::rejected or
  ///     Unanswered::Reason::unavailable as the store's failure maps.
  [[nodiscard]] auto answer_for(const ProductId& product)
      -> Expected<StorefrontAnswer, Unanswered>;
  /// @brief Asks about a concept with its pricing.
  ///
  /// Blocks on two store requests, renewing the token once as above. When no
  /// edition is the product itself, as for a cloud-only product the store has
  /// no listing for, an edition of the same title (the same publisher and
  /// title id) answers instead, preferring one that states its streaming
  /// support.
  ///
  /// @param product The product the answer is for.
  /// @param grouped_under The concept to ask about.
  /// @return As the single-argument overload.
  [[nodiscard]] auto answer_for(const ProductId& product,
                                const ConceptId& grouped_under)
      -> Expected<StorefrontAnswer, Unanswered>;

 private:
  /// @brief Opens the store app and asks one question, renewing the token once
  /// when the store refuses it.
  ///
  /// @tparam Ask A callable taking the authenticated client and returning
  ///     Expected<psn::graphql::TitleDetails, psn::graphql::GraphqlError>.
  /// @param ask The question.
  /// @return The details, or the mapped failure.
  template <class Ask>
  [[nodiscard]] auto asked_of_the_store(const Ask& ask)
      -> Expected<psn::graphql::TitleDetails, Unanswered>;
  /// @brief Builds the answer from the store's details.
  ///
  /// @param product The product asked about.
  /// @param details The store's details.
  /// @return The editions whose product ids parse, each with its streaming
  ///     support (when the store gave notices), notice, ownership,
  ///     add-to-library offer and whether a trial_actions offer was made.
  auto answered(const ProductId& product,
                const psn::graphql::TitleDetails& details) const
      -> StorefrontAnswer;
  /// @brief Finds an edition of the same title as a product none of the
  /// editions is.
  ///
  /// @param product The product asked about.
  /// @param details The store's details.
  /// @return The first edition sharing the product's publisher and title id
  ///     that states its streaming support, else the first sharing them;
  ///     nothing when one is the product itself or none shares them.
  static auto same_title(const ProductId& product,
                         const psn::graphql::TitleDetails& details)
      -> std::optional<ProductId>;
  /// @brief Logs the calls to action the store offered each edition, so
  /// that per-account offers such as game trials can be read from the log.
  ///
  /// @param details The store's details.
  void note_offers(const psn::graphql::TitleDetails& details) const;

  /// @brief Maps a store failure to a catalog failure.
  ///
  /// @param error The failure.
  /// @return A cancellation; store access lapsed for a refused or missing
  /// token;
  ///     Unanswered::Reason::malformed for partial or unreadable replies;
  ///     Unanswered::Reason::not_listed for not found;
  ///     Unanswered::Reason::rejected for refused requests;
  ///     Unanswered::Reason::unavailable for transport and server errors.
  static auto unanswered(const psn::graphql::GraphqlError& error) noexcept
      -> Unanswered;

  /// @brief The user; not owned.
  users::PsUser& user_;
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps answers.
  WallStamp stamp_;
};

}  // namespace libchiaking::catalog::ps
