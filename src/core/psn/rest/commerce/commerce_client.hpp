/// @file
/// @brief Declares CommerceClient, the Kamaji commerce service's owned
/// entitlement records, read with a commerce token for owned entitlements.
///
/// These records are what the LegacyCloudStream catalogue and checkout know the
/// account owns; their PS5 records are ignored. Each record is kept as raw JSON
/// for the catalogue to read.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief The diagnostic label of the entitlement list request.
inline constexpr std::string_view commerce_entitlements_label =
    "psn.commerce.internal_entitlements";
/// @brief The diagnostic label of the single entitlement request.
inline constexpr std::string_view commerce_entitlement_label =
    "psn.commerce.internal_entitlement";
/// @brief How many records one page asks for.
inline constexpr int commerce_entitlements_page_size = 300;
/// @brief The most pages read before the list counts as truncated.
inline constexpr int commerce_entitlements_page_limit = 200;
/// @brief The longest entitlement id put in a URL, in bytes; longer ids are
/// cut.
inline constexpr std::size_t commerce_entitlement_id_capacity = 256;

/// @brief Why a commerce request failed, with the reply's status, how many
/// requests were made and, for a failure to send, the transport failure. An
/// immutable value.
class CommerceError {
 public:
  /// @brief Why the request failed.
  enum class Reason : std::uint8_t {
    /// @brief The stop token fired.
    cancelled,
    /// @brief The request did not complete, or the service answered 5xx or
    /// another non-4xx error.
    unavailable,
    /// @brief The service refused the token (HTTP 401 or 403).
    auth_refused,
    /// @brief HTTP 404.
    not_found,
    /// @brief Another 4xx, or a single-entitlement reply with a 2xx status
    /// other than 200.
    rejected,
    /// @brief A list page could not be read.
    malformed,
    /// @brief The list did not end within commerce_entitlements_page_limit
    /// pages.
    truncated,
  };

  /// @brief Wraps a failure to send.
  ///
  /// @param failure The client's failure; cancelled when it was a
  ///     cancellation, unavailable otherwise.
  /// @param requests How many requests were made.
  explicit CommerceError(net::http::Failure failure, int requests = 1) noexcept;
  /// @brief Makes a failure.
  ///
  /// @param reason Why.
  /// @param http_status The reply's status.
  /// @param requests How many requests were made.
  CommerceError(Reason reason, std::uint16_t http_status,
                int requests = 1) noexcept;

  /// @brief Classifies an error status.
  ///
  /// @param http_status The status.
  /// @return auth_refused for 401 and 403, not_found for 404, rejected for
  ///     other 4xx, unavailable otherwise.
  static auto from_status(std::uint16_t http_status) noexcept -> CommerceError;

  /// @brief Reads why.
  ///
  /// @return The reason.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Reads the reply's status.
  ///
  /// @return It, or 0 for a failure to send.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief Reads how many requests were made, the failing one included.
  ///
  /// @return The count.
  auto requests() const noexcept -> int { return requests_; }
  /// @brief Reads the failure to send.
  ///
  /// @return It, or nothing when the service answered.
  auto failure() const noexcept -> const std::optional<net::http::Failure>& {
    return failure_;
  }
  /// @brief Names the reason for logs.
  ///
  /// @return The enumerator's name.
  auto name() const noexcept -> std::string_view;

 private:
  /// @brief Why.
  Reason reason_;
  /// @brief The reply's status; 0 for a failure to send.
  std::uint16_t http_status_{};
  /// @brief Requests made.
  int requests_;
  /// @brief The failure to send.
  std::optional<net::http::Failure> failure_;
};

/// @brief Every owned entitlement record. Plain data.
struct Entitlements {
  /// @brief The records, each as raw JSON, in page order.
  std::vector<glz::raw_json> items;
  /// @brief How many pages were requested.
  int requests{};
  /// @brief The last page's status.
  std::uint16_t last_status{};
};

/// @brief One entitlement looked up. Plain data.
struct EntitlementLookup {
  /// @brief True for a 200 reply; false for 404, meaning the account does not
  /// own it.
  bool owned{};
  /// @brief The reply's status, 200 or 404.
  std::uint16_t http_status{};
  /// @brief The record when owned: the entry of the reply's `entitlements`
  /// whose `id` or `product_id` is the id asked for, or the whole body when
  /// it is itself a record with an `id`; nothing when none matches.
  std::optional<glz::raw_json> entitlement;
};

/// @brief Told the outcome of each list page as it lands: its 1-based number
/// and its status or failure. On a failure to send, the page number given is
/// the count of requests made.
using EntitlementPageObserver = std::function<void(
    int page, const Expected<std::uint16_t, CommerceError>& outcome)>;

/// @brief Reads owned entitlement records from
/// `commerce.api.np.km.playstation.net`.
///
/// Owns a copy of the HTTP client (whose provider must outlive it). Each call
/// blocks for its requests. Not for concurrent use.
class CommerceClient {
 public:
  /// @brief Prepares the requests.
  ///
  /// @param client The client the requests go through.
  /// @param user_agent The user agent internal_entitlement() sends; none when
  ///     empty; must outlive this object.
  explicit CommerceClient(net::http::Client client,
                          std::string_view user_agent = {}) noexcept;

  /// @brief Reads every owned entitlement record, page by page.
  ///
  /// GET
  /// `.../commerce/api/v1/users/me/internal_entitlements?fields=game_meta,
  /// sku_meta&entitlement_type=5&start=<n>&size=300` until a page holds fewer
  /// than 300 records, waiting 100 ms between pages; a page's `entitlements`
  /// are kept raw. The user agent is not sent.
  ///
  /// @param bearer The commerce token for owned entitlements.
  /// @param observe Told each page's outcome; may be empty.
  /// @return Every record.
  /// @retval CommerceError::Reason::cancelled The stop token fired, also
  ///     during the wait between pages.
  /// @retval CommerceError::Reason::truncated 200 pages were read and the
  ///     list had not ended.
  /// @retval CommerceError A page's error status, unreadable page or failure
  ///     to send.
  [[nodiscard]] auto internal_entitlements(
      const auth::BearerToken& bearer,
      const EntitlementPageObserver& observe = {})
      -> Expected<Entitlements, CommerceError>;
  /// @brief Looks one owned entitlement up.
  ///
  /// GET `.../internal_entitlements/<id>?fields=game_meta,sku_meta`, with the
  /// user agent when one was given.
  ///
  /// @param bearer The commerce token.
  /// @param id The entitlement id; cut to commerce_entitlement_id_capacity
  ///     bytes.
  /// @return Owned with its record for 200, not owned for 404.
  /// @retval CommerceError::Reason::rejected A 2xx other than 200, or a 4xx
  ///     other than 401, 403 and 404.
  /// @retval CommerceError Another error status or a failure to send.
  [[nodiscard]] auto internal_entitlement(const auth::BearerToken& bearer,
                                          std::string_view id)
      -> Expected<EntitlementLookup, CommerceError>;

 private:
  /// @brief The account's owned entitlements.
  static constexpr std::string_view entitlements_url =
      "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/"
      "internal_entitlements";
  /// @brief The list request, logged with its query.
  static constexpr net::http::Endpoint entitlements_endpoint{
      .label = commerce_entitlements_label, .log_query = true};
  /// @brief The single entitlement request.
  static constexpr net::http::Endpoint entitlement_endpoint{
      .label = commerce_entitlement_label};

  /// @brief 300 records a page, at most 200 pages, 100 ms apart.
  static constexpr net::http::Paging entitlement_paging{
      .page_size = commerce_entitlements_page_size,
      .page_limit = commerce_entitlements_page_limit,
      .delay = std::chrono::milliseconds{100},
  };

  /// @brief A string field tolerated when malformed.
  using Text = json::ps::Tolerant<std::string>;

  /// @brief A list page's `entitlements`.
  struct Listed {
    /// @brief The page's records, raw.
    json::ps::Tolerant<std::vector<glz::raw_json>> entitlements;
  };
  /// @brief The ids a record is matched by.
  struct Identified {
    /// @brief The record's `id`.
    Text id;
    /// @brief The record's `product_id`.
    Text product_id;
  };
  /// @brief A single entitlement reply: a list of records, or one record.
  struct Looked {
    /// @brief The records, when the reply is a list.
    json::ps::Tolerant<std::vector<glz::raw_json>> entitlements;
    /// @brief The `id`, when the reply is one record.
    Text id;
  };

  /// @brief Reads a list page.
  ///
  /// @param response The reply.
  /// @return The page; empty when the body is not an object.
  /// @retval CommerceError The status's classification for a non-2xx reply.
  /// @retval CommerceError::Reason::malformed The body could not be read.
  [[nodiscard]] static auto document(const net::http::Response& response)
      -> Expected<json::ps::Tolerant<Listed>, CommerceError>;
  /// @brief Finds the record for an id in a single entitlement reply.
  ///
  /// @param response The reply.
  /// @param id The id asked for.
  /// @return The record, as EntitlementLookup::entitlement describes.
  [[nodiscard]] static auto matching(const net::http::Response& response,
                                     std::string_view id)
      -> std::optional<glz::raw_json>;
  /// @brief Tells whether a field holds an id.
  ///
  /// @param field The field.
  /// @param id The id.
  /// @return True when the field is present and equal to `id`.
  static auto names(const Text& field, std::string_view id) -> bool;

  /// @brief The client the requests go through.
  net::http::Client client_;
  /// @brief The user agent for the single entitlement request; none when
  /// empty.
  std::string_view user_agent_;
};

}  // namespace libchiaking::psn::rest
