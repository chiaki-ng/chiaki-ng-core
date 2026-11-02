/// @file
/// @brief Declares AuthenticatedClient, the GraphQL calls made
/// with the user's app token: a game concept's details and pricing, a
/// product's concept, and the user's PlayStation Plus subscriptions.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/authenticated/subscription.hpp"
#include "src/core/psn/graphql/authenticated/title_details.hpp"
#include "src/core/psn/graphql/content_rating.hpp"
#include "src/core/psn/graphql/error.hpp"
#include "src/core/psn/graphql/genres.hpp"
#include "src/core/psn/graphql/release_date.hpp"
#include "src/core/psn/graphql/reply_integer.hpp"
#include "src/core/psn/graphql/reply_strings.hpp"
#include "src/core/psn/graphql/reply_text.hpp"
#include "src/core/psn/graphql/retrieved.hpp"
#include "src/core/psn/graphql/sender.hpp"
#include "src/core/psn/graphql/stats.hpp"
#include "src/core/psn/graphql/title_media.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::psn::auth {
/// @brief Where a session keeps the app token.
class AppTokenSlot;
}  // namespace libchiaking::psn::auth

namespace libchiaking::psn::graphql {

/// @brief Sends the authenticated GraphQL operations and reads their replies
/// into the domain's shapes.
///
/// Every reply field is tolerated when malformed; a reply is refused only when
/// what the call is for is missing. Each call blocks for one request with one
/// retry a second later; a 401 or 403 is recorded against the app token. Holds
/// references to the HTTP client, the app token slot and the statistics, which
/// must outlive it. Not for concurrent use.
class AuthenticatedClient {
 public:
  /// @brief Prepares the calls.
  ///
  /// @param client The client the requests go through.
  /// @param credentials The session's app token slot.
  /// @param stats Where calls are recorded; may be null.
  AuthenticatedClient(net::http::Client& client,
                      auth::AppTokenSlot& credentials,
                      Stats* stats = nullptr) noexcept;

  /// @brief Reads a concept by id: `metGetConceptById` with
  /// `{"conceptId":<id>}`.
  ///
  /// @param concept_id The concept id.
  /// @param locale The store locale, sent as `x-psn-store-locale-override`.
  /// @return The concept's summary and default product, unpriced.
  /// @retval GraphqlError::Kind::not_found `conceptRetrieve` was null.
  /// @retval GraphqlError::Kind::malformed The concept is unreadable, has an
  ///     invalid id, or is another concept.
  /// @retval GraphqlError The failures of Sender::send_authenticated().
  [[nodiscard]] auto concept_by_id(std::string_view concept_id,
                                   std::string_view locale)
      -> Expected<TitleDetails, GraphqlError>;
  /// @brief Adds a concept's products, offers and the user's ownership:
  /// `metGetPricingDataByConceptId` with `{"conceptId":<id>}`.
  ///
  /// @param details The concept, from concept_by_id().
  /// @param locale The store locale.
  /// @return The details, priced.
  /// @retval GraphqlError::Kind::malformed The reply is another concept or
  ///     lacks `ownedTitles`, `selectableProducts.purchasedTitles` or
  ///     `purchasableProducts`.
  /// @retval GraphqlError The failures of Sender::send_authenticated().
  [[nodiscard]] auto concept_pricing(TitleDetails details,
                                     std::string_view locale)
      -> Expected<TitleDetails, GraphqlError>;
  /// @brief Reads a product with its concept, the concept's products and the
  /// user's ownership: `metGetConceptByProductIdQuery` with
  /// `{"productId":<id>}`. The product's own offers are attached to it.
  ///
  /// @param product_id A product or SKU id.
  /// @param locale The store locale.
  /// @return The details, priced.
  /// @retval GraphqlError::Kind::not_found `productRetrieve` was absent or
  ///     null.
  /// @retval GraphqlError::Kind::malformed The reply names another product,
  ///     has no readable concept or ownership, or does not list the product.
  /// @retval GraphqlError The failures of Sender::send_authenticated().
  [[nodiscard]] auto concept_by_product(std::string_view product_id,
                                        std::string_view locale)
      -> Expected<TitleDetails, GraphqlError>;
  /// @brief Reads the user's active PlayStation Plus subscriptions:
  /// `getSubscriptions` with no variables, on the web gateway.
  ///
  /// @return The active `PS_PLUS` subscriptions.
  /// @retval GraphqlError::Kind::malformed `fetchSubscriptions.subscriptions`
  ///     is missing, or an entry lacks its type, or a PlayStation Plus entry
  ///     its `active` flag, or an active one its tier or id.
  /// @retval GraphqlError The failures of Sender::send_authenticated().
  [[nodiscard]] auto subscriptions()
      -> Expected<PlusSubscriptions, GraphqlError>;

 private:
  /// @brief A GraphQL failure's kind.
  using Kind = GraphqlError::Kind;
  /// @brief The compatibility notice type that speaks of cloud streaming.
  static constexpr std::string_view streaming_notice = "STREAMING_SUPPORTED";
  /// @brief One retry, a second after a failed attempt.
  static constexpr net::http::Retry check_retry{
      .retries = 1, .delay = std::chrono::milliseconds{1000}};
  /// @brief The concept request.
  static constexpr net::http::Endpoint concept_endpoint{
      .label = authenticated::concept_by_id.label, .retry = check_retry};
  /// @brief The pricing request.
  static constexpr net::http::Endpoint pricing_endpoint{
      .label = authenticated::concept_pricing.label, .retry = check_retry};
  /// @brief The product request.
  static constexpr net::http::Endpoint product_endpoint{
      .label = authenticated::concept_by_product.label, .retry = check_retry};

  /// @brief The subscriptions request.
  static constexpr net::http::Endpoint subscriptions_endpoint{
      .label = authenticated::get_subscriptions.label,
      .retry = check_retry,
  };

  /// @brief A field tolerated when malformed.
  ///
  /// @tparam Value The field's type.
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;
  /// @brief A list whose entries and itself are tolerated when malformed.
  ///
  /// @tparam Value An entry's type.
  template <class Value>
  using TolerantList = Tolerant<std::vector<Tolerant<Value>>>;

  /// @brief A compatibility notice.
  struct Notice {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `type`, such as `STREAMING_SUPPORTED`.
    Tolerant<std::string> type;
    /// @brief Its `targetPlatforms`.
    ReplyStrings target_platforms;
    /// @brief Its `value`.
    ReplyText value;
  };
  /// @brief `compatibilityNoticesByPlatform`: notices keyed by platform.
  using NoticesByPlatform =
      Tolerant<std::vector<std::pair<std::string, TolerantList<Notice>>>>;

  /// @brief A call to action's parameter.
  struct Param {
    /// @brief Its `name`, such as `skuId`.
    Tolerant<std::string> name;
    /// @brief Its `values`.
    ReplyStrings values;
  };
  /// @brief A call to action's `action`.
  struct Action {
    /// @brief Its `type`.
    ReplyText type;
    /// @brief Its `param` list.
    TolerantList<Param> param;
  };
  /// @brief An ineligibility reason.
  struct Reason {
    /// @brief Its `type`.
    ReplyText type;
  };
  /// @brief A call to action's `meta`.
  struct Meta {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The `preOrder` flag.
    Tolerant<bool> pre_order;
    /// @brief The `ineligibilityReasons`.
    TolerantList<Reason> ineligibility_reasons;
  };
  /// @brief A call to action's `price`.
  struct Price {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `basePrice`, as shown.
    ReplyText base_price;
    /// @brief `discountedPrice`, as shown.
    ReplyText discounted_price;
    /// @brief `currencyCode`.
    ReplyText currency_code;
    /// @brief `basePriceValue`, in the minor unit.
    ReplyInteger<std::int64_t> base_price_value;
    /// @brief `discountedValue`, in the minor unit.
    ReplyInteger<std::int64_t> discounted_value;
    /// @brief `isFree`.
    Tolerant<bool> is_free;
    /// @brief `isTiedToSubscription`.
    Tolerant<bool> is_tied_to_subscription;
    /// @brief `serviceBranding`.
    ReplyStrings service_branding;
  };
  /// @brief A call to action, one of `mobilectas`.
  struct Cta {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `type`.
    ReplyText type;
    /// @brief Its `cloudStreamingMessage`.
    ReplyText cloud_streaming_message;
    /// @brief Its `action`.
    Tolerant<Action> action;
    /// @brief Its `meta`.
    Tolerant<Meta> meta;
    /// @brief Its `price`.
    Tolerant<Price> price;
  };
  /// @brief A product description.
  struct Description {
    /// @brief Its `type`.
    Tolerant<std::string> type;
    /// @brief Its `value`.
    ReplyText value;
  };
  /// @brief A product's `edition`.
  struct Edition {
    /// @brief Its `name`.
    ReplyText name;
  };
  /// @brief A product as sent.
  struct Product {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `id`.
    ReplyText id;
    /// @brief Its `name`.
    ReplyText name;
    /// @brief Its `npTitleId`.
    ReplyText np_title_id;
    /// @brief Its `platforms`.
    ReplyStrings platforms;
    /// @brief Its `releaseDate`.
    ReleaseDate release_date;
    /// @brief Its `storeDisplayClassification`.
    ReplyText store_display_classification;
    /// @brief Its `edition`.
    Tolerant<Edition> edition;
    /// @brief Its compatibility notices.
    NoticesByPlatform compatibility_notices_by_platform;
    /// @brief Its `descriptions`.
    TolerantList<Description> descriptions;
    /// @brief Its calls to action.
    TolerantList<Cta> mobilectas;
    /// @brief Its `media`.
    MediaList media;
  };
  /// @brief An owned or purchased title given as an object.
  struct Titled {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `productId`.
    ReplyText product_id;
    /// @brief Its `id`.
    ReplyText id;
    /// @brief Its `entitlementId`.
    ReplyText entitlement_id;
    /// @brief Its `titleId`.
    ReplyText title_id;
    /// @brief Its `skuId`.
    ReplyText sku_id;
    /// @brief Its `product`.
    Tolerant<Product> product;
  };
  /// @brief A list of owned or purchased titles, each a bare id or a Titled.
  using Identities =
      Tolerant<std::vector<Tolerant<std::variant<std::string, Titled>>>>;
  /// @brief A concept's `selectableProducts`.
  struct Selectable {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `purchasedTitles`.
    Identities purchased_titles;
    /// @brief `purchasableProducts`.
    TolerantList<Product> purchasable_products;
  };
  /// @brief A concept as sent.
  struct Concept {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `id`.
    ReplyText id;
    /// @brief Its `name`.
    ReplyText name;
    /// @brief Its `publisherName`.
    ReplyText publisher_name;
    /// @brief Its `releaseDate`.
    ReleaseDate release_date;
    /// @brief Its `combinedLocalizedGenres`.
    Genres combined_localized_genres;
    /// @brief Its `contentRating`.
    Tolerant<ContentRating> content_rating;
    /// @brief Its compatibility notices.
    NoticesByPlatform compatibility_notices_by_platform;
    /// @brief Its `media`.
    MediaList media;
    /// @brief Its `defaultProduct`.
    Tolerant<Product> default_product;
    /// @brief Its `ownedTitles`.
    Identities owned_titles;
    /// @brief Its `selectableProducts`.
    Tolerant<Selectable> selectable_products;
  };
  /// @brief The concept operations' `data`.
  struct ConceptData {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `conceptRetrieve`.
    Retrieved<Concept> concept_retrieve;
  };
  /// @brief `productRetrieve`: the product with its `concept`.
  struct ProductRoot {
    /// @brief The product's `id`.
    ReplyText id;
    /// @brief The product's `concept`.
    Tolerant<Concept> grouped;
    /// @brief The product's calls to action.
    TolerantList<Cta> mobilectas;

    /// @brief Maps the `concept` key to grouped.
    struct glaze {
      /// @brief The mapped type.
      using T = ProductRoot;
      /// @brief The key mapping.
      static constexpr auto value = glz::object(
          "id", &T::id, "concept", &T::grouped, "mobilectas", &T::mobilectas);
    };
  };
  /// @brief The product operation's `data`.
  struct ProductData {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `productRetrieve`.
    Retrieved<ProductRoot> product_retrieve;
  };
  /// @brief A subscription as sent.
  struct Subscription {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief Its `subscriptionType`, such as `PS_PLUS`.
    Tolerant<std::string> subscription_type;
    /// @brief Its `active` flag.
    Tolerant<bool> active;
    /// @brief Its `tierLabel`.
    Tolerant<std::string> tier_label;
    /// @brief Its `id`, the SKU.
    Tolerant<std::string> id;
    /// @brief Its `productName`.
    Tolerant<std::string> product_name;
    /// @brief Its `endDate`.
    Tolerant<std::string> end_date;
  };
  /// @brief `fetchSubscriptions`.
  struct Subscriptions {
    /// @brief Its `subscriptions`.
    std::optional<std::vector<Tolerant<Subscription>>> subscriptions;
  };
  /// @brief The subscriptions operation's `data`.
  struct SubscriptionsData {
    /// @brief JSON keys are the camel-case forms of the member names.
    using keys = json::ps::CamelCaseKeys;
    /// @brief `fetchSubscriptions`.
    std::optional<Subscriptions> fetch_subscriptions;
  };

  /// @brief Keeps the active PlayStation Plus subscriptions.
  ///
  /// @param data The reply, moved from.
  /// @return The active ones; nothing when the reply fails the checks
  ///     subscriptions() lists.
  [[nodiscard]] static auto active_plus_subscriptions(SubscriptionsData& data)
      -> std::optional<std::vector<ActivePlusSubscription>>;
  /// @brief Tells whether a streaming notice targets the PS5.
  ///
  /// @param platforms The notice's target platforms.
  /// @return True when `PS5` or `DWC` is listed.
  static auto streams_on_ps5(const std::vector<std::string>& platforms) -> bool;
  /// @brief Reads what the compatibility notices say about streaming.
  ///
  /// @param notices The notices, moved from.
  /// @return The verdict TitleStreaming describes.
  static auto streaming_of(NoticesByPlatform& notices) -> TitleStreaming;
  /// @brief Copies an action parameter the offer cares about.
  ///
  /// @param offer The offer.
  /// @param param The parameter, moved from.
  static void take_param(TitleOffer& offer, Param& param);
  /// @brief Reads a call to action.
  ///
  /// @param cta The call to action, moved from.
  /// @return The offer.
  static auto offer_of(Cta& cta) -> TitleOffer;
  /// @brief Reads the calls to action.
  ///
  /// @param ctas The list, moved from.
  /// @return The readable offers, in order.
  static auto offers_of(TolerantList<Cta>& ctas) -> std::vector<TitleOffer>;
  /// @brief Reads a product.
  ///
  /// @param value The product, moved from.
  /// @return The product; nothing when it is missing or has no id.
  [[nodiscard]] static auto product_of(Tolerant<Product>& value)
      -> std::optional<TitleProduct>;
  /// @brief Reads owned or purchased titles into ids.
  ///
  /// @param values The list, moved from.
  /// @return Each bare id, and every non-empty id of each object; nothing
  ///     when the list is missing.
  [[nodiscard]] static auto identities_of(Identities& values)
      -> std::optional<std::vector<std::string>>;
  /// @brief Reads the user's ownership of a concept.
  ///
  /// @param grouped The concept, moved from.
  /// @return The ownership; nothing when `ownedTitles`, `purchasedTitles` or
  ///     `purchasableProducts` is missing.
  [[nodiscard]] static auto ownership_of(Concept& grouped)
      -> std::optional<TitleDetails::Ownership>;
  /// @brief Reads a concept's summary and default product.
  ///
  /// @param grouped The concept, moved from.
  /// @return The details; nothing when its id is invalid.
  [[nodiscard]] static auto concept_details(Concept& grouped)
      -> std::optional<TitleDetails>;
  /// @brief Adds a pricing reply to details.
  ///
  /// @param details The details.
  /// @param grouped The reply's concept, moved from.
  /// @return The priced details; nothing when it is another concept or has no
  ///     ownership.
  [[nodiscard]] static auto priced(TitleDetails details, Concept& grouped)
      -> std::optional<TitleDetails>;
  /// @brief Reads a product reply.
  ///
  /// @param root The reply, moved from.
  /// @param product_id The product or SKU id asked for.
  /// @return The priced details; nothing when the reply is for another
  ///     product, or lacks the concept, its ownership or the product.
  [[nodiscard]] static auto by_product(ProductRoot& root,
                                       std::string_view product_id)
      -> std::optional<TitleDetails>;
  /// @brief Builds variables of one string field.
  ///
  /// @param key The field's name.
  /// @param value Its value, up to any NUL.
  /// @return The variables.
  static auto single_field(std::string_view key, std::string_view value)
      -> Variables;
  /// @brief Makes the failure of a reply that did not carry what was asked.
  ///
  /// @param operation The operation's name.
  /// @param kind Why.
  /// @param result The reply.
  /// @return The failure, with the reply's status and attempts.
  static auto unparsed(std::string_view operation, Kind kind,
                       const Result& result) -> GraphqlError;

  /// @brief Sends the operations.
  Sender sender_;
  /// @brief The session's app token slot.
  auth::AppTokenSlot& credentials_;
};

}  // namespace libchiaking::psn::graphql
