/// @file
/// @brief Declares KamajiClient, the PlayStation Now (Kamaji) store: its
/// session, catalogue containers and
/// categories, a title's store container, and the buy-now checkout.
///
/// A Kamaji session is opened by redeeming a LegacyCloudStream authorisation
/// code at `psnow.playstation.com/kamaji/api/pcnow/00_09_000/user/session`; the
/// store answers with a `JSESSIONID` cookie that every later session request
/// sends. The public store containers under
/// `/store/api/pcnow/00_09_000/container/` need no session.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/response_document.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/rest/store/store_error.hpp"
#include "src/core/psn/user_agents.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief The diagnostic label of the session request.
inline constexpr std::string_view kamaji_create_session_label =
    "psn.kamaji.create_session";
/// @brief The diagnostic label of the stores request.
inline constexpr std::string_view kamaji_stores_label = "psn.kamaji.stores";
/// @brief The diagnostic label of container requests.
inline constexpr std::string_view kamaji_container_label =
    "psn.kamaji.container";
/// @brief The diagnostic label of category requests.
inline constexpr std::string_view kamaji_category_label = "psn.kamaji.category";
/// @brief The diagnostic label of the checkout preview.
inline constexpr std::string_view kamaji_buynow_preview_label =
    "psn.kamaji.buynow_preview";
/// @brief The diagnostic label of the checkout.
inline constexpr std::string_view kamaji_buynow_label = "psn.kamaji.buynow";
/// @brief The diagnostic name of the session cookie.
inline constexpr std::string_view kamaji_session_credential = "kamaji session";
/// @brief One more than the largest session request body sent, in bytes.
inline constexpr std::size_t kamaji_session_body_capacity = 512;
/// @brief The longest session cookie kept, in bytes.
inline constexpr std::size_t kamaji_session_cookie_capacity = 511;
/// @brief The longest country or language put in a title container URL, in
/// bytes.
inline constexpr std::size_t kamaji_locale_component_capacity = 64;
/// @brief The longest title id put in a title container URL, in bytes.
inline constexpr std::size_t kamaji_title_id_capacity = 256;

/// @brief The store's answer to a session request. Plain data.
struct KamajiSession {
  /// @brief The reply's status.
  std::uint16_t http_status{};
  /// @brief Whether the reply's `header.status_code` was kamaji_status_ok.
  bool accepted{};
  /// @brief The account's store country, `data.country`; empty when absent.
  std::string country;
  /// @brief The account's store language, `data.language`; empty when absent.
  std::string language;
};

/// @brief One page of a store container or category. Plain data.
struct ContainerPage {
  /// @brief The `total_results`, clamped to `int`; 0 when it is not a number;
  /// nothing when absent.
  std::optional<int> total_results;
  /// @brief The `links` that are JSON objects, raw, in reply order.
  std::vector<glz::raw_json> links;
};

/// @brief What checking a SKU out would cost. Plain data.
struct CheckoutPreview {
  /// @brief Whether the cart's `total_price_value` is zero or absent.
  bool free{};
  /// @brief The cart's `total_price` as shown, for a priced cart; empty
  /// otherwise.
  std::string display_price;
  /// @brief The first item's `sku_id`, for a free cart; empty otherwise.
  std::string sku;
};

/// @brief Requests to the LegacyCloudStream store, holding the Kamaji session
/// cookie once one is opened.
///
/// Owns a copy of the HTTP client (whose provider must outlive it) and the
/// cookie; with_same_session() shares the cookie with another client. Each call
/// blocks for one round trip. Not for concurrent use; open copies for
/// concurrent callers.
class KamajiClient {
 public:
  /// @brief Prepares the requests, with no session.
  ///
  /// @param client The client the requests go through.
  explicit KamajiClient(net::http::Client client) noexcept;

  /// @brief Tells whether a session cookie is held.
  ///
  /// @return True once the store has set `JSESSIONID`.
  auto has_session() const noexcept -> bool { return !session_.empty(); }
  /// @brief Makes another client on the same session.
  ///
  /// @param client The client the copy's requests go through.
  /// @return A client holding a copy of the session cookie.
  auto with_same_session(net::http::Client client) const -> KamajiClient;

  /// @brief Opens a Kamaji session by redeeming a LegacyCloudStream
  /// authorisation code.
  ///
  /// POST `.../kamaji/api/pcnow/00_09_000/user/session` with the form
  /// `code=<code>&client_id=<legacy_cloud_stream_client_id>&duid=<duid>` as
  /// `text/plain`, the LegacyCloudStream user agent and its `Origin`, `Referer`
  /// and `X-Alt-Referer`. A `JSESSIONID` cookie in the reply is kept, whatever
  /// the status.
  ///
  /// @param code The code, from
  /// Authenticator::legacy_cloud_stream_catalog_code() or a
  ///     sibling.
  /// @param duid The device's DUID.
  /// @return The status, whether the store accepted, and the account's
  ///     country and language; a refusal is a value, not a failure.
  /// @retval StoreError::Reason::failed The form is 512 bytes or more (an
  ///     invalid request; nothing sent), or the request did not complete.
  [[nodiscard]] auto create_session(const auth::AuthCode& code,
                                    std::string_view duid)
      -> Expected<KamajiSession, StoreError>;
  /// @brief Finds the session's store root.
  ///
  /// GET `.../user/stores` with the session cookie; `data.base_url` is read.
  ///
  /// @return The base URL of the root container.
  /// @retval StoreError::Reason::refused The status is not 200.
  /// @retval StoreError::Reason::malformed The reply is unreadable, not
  ///     accepted, or has no base URL.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto stores() -> Expected<std::string, StoreError>;
  /// @brief Reads a container on the session.
  ///
  /// GET `<base_url>?size=<size>` with the session cookie.
  ///
  /// @param base_url The container's URL, from stores() or a link.
  /// @param size How many links to ask for.
  /// @return The page.
  /// @retval StoreError The failures of document().
  [[nodiscard]] auto container(std::string_view base_url, int size)
      -> Expected<ContainerPage, StoreError>;
  /// @brief Reads a page of a public store container, without a session.
  ///
  /// GET
  /// `.../store/api/pcnow/00_09_000/container/<country>/<language>/19/<id>?useOffers=true&gkb=1&gkb2=1&start&size`
  /// with the LegacyCloudStream user agent. The country, language and id are
  /// put in the path as given.
  ///
  /// @param country The store country.
  /// @param language The store language.
  /// @param container_id The container.
  /// @param start Index of the first link.
  /// @param size How many links to ask for.
  /// @return The page.
  /// @retval StoreError The failures of document().
  [[nodiscard]] auto container_page(std::string_view country,
                                    std::string_view language,
                                    std::string_view container_id, int start,
                                    int size)
      -> Expected<ContainerPage, StoreError>;
  /// @brief A started container page, decoded when collected.
  class PendingPage;
  /// @brief Starts reading a page of a public store container, without
  /// waiting for it.
  ///
  /// The request is the one container_page() sends.
  ///
  /// @param country The store country.
  /// @param language The store language.
  /// @param container_id The container.
  /// @param start Index of the first link.
  /// @param size How many links to ask for.
  /// @return The started page; dropping it uncollected abandons it.
  [[nodiscard]] auto start_container_page(std::string_view country,
                                          std::string_view language,
                                          std::string_view container_id,
                                          int start, int size) -> PendingPage;
  /// @brief Reads a title's public store container, as a LegacyCloudStream
  /// launch does to find its SKUs.
  ///
  /// GET
  /// `.../container/<country>/<language>/19/<title_id>?useOffers=true&gkb=1&gkb2=1`
  /// with the browser user agent, each path part percent-encoded and cut to
  /// its capacity.
  ///
  /// @tparam Reply The shape the container is decoded into.
  /// @param country The store country.
  /// @param language The store language.
  /// @param title_id The title.
  /// @return The decoded container.
  /// @retval StoreError The failures of document().
  template <class Reply>
  [[nodiscard]] auto title_container(std::string_view country,
                                     std::string_view language,
                                     std::string_view title_id)
      -> Expected<Reply, StoreError> {
    auto sent = send(title_container_request(country, language, title_id));
    if (!sent) return unexpected(sent.error());
    return document<Reply>(sent.value());
  }
  /// @brief Reads a page of a store category, without a session.
  ///
  /// GET `<category_url>?start=<start>&size=<size>` with the browser user
  /// agent.
  ///
  /// @param category_url The category's URL, from a container link.
  /// @param start Index of the first link.
  /// @param size How many links to ask for.
  /// @return The page.
  /// @retval StoreError The failures of document().
  [[nodiscard]] auto category(std::string_view category_url, int start,
                              int size) -> Expected<ContainerPage, StoreError>;
  /// @brief Asks what checking a SKU out would cost.
  ///
  /// POST `.../user/checkout/buynow/preview` with the form `sku=<sku>`, the
  /// checkout token as a bearer token and, when a session is held, its
  /// cookie. A new session cookie in the reply is kept.
  ///
  /// @param bearer The LegacyCloudStream checkout token.
  /// @param sku The SKU.
  /// @return The preview.
  /// @retval StoreError::Reason::refused The status is not 200 or the Kamaji
  ///     status code is not kamaji_status_ok.
  /// @retval StoreError::Reason::malformed The reply has no cart or an
  ///     unreadable price.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto buynow_preview(const auth::BearerToken& bearer,
                                    std::string_view sku)
      -> Expected<CheckoutPreview, StoreError>;
  /// @brief Checks a SKU out to resolve an entitlement the preview showed to
  /// be free.
  ///
  /// POST `.../user/checkout/buynow` with the form
  /// `sku=<sku>&skipEmail=true`, the checkout token and, when held, the
  /// session cookie. A new session cookie in the reply is kept.
  ///
  /// @param bearer The LegacyCloudStream checkout token.
  /// @param sku The SKU.
  /// @return Nothing, when the store answered 200 with kamaji_status_ok.
  /// @retval StoreError::Reason::refused Any other answer.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto buynow(const auth::BearerToken& bearer,
                            std::string_view sku) -> Expected<void, StoreError>;

 private:
  /// @brief The Kamaji API root.
  static constexpr std::string_view api_url =
      "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000";
  /// @brief The public store containers.
  static constexpr std::string_view store_container_url =
      "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/";
  /// @brief The session cookie's name.
  static constexpr std::string_view session_cookie = "JSESSIONID";
  /// @brief The LegacyCloudStream user agent.
  static constexpr std::string_view app_user_agent =
      legacy_cloud_stream_http_user_agent;
  /// @brief The browser user agent.
  static constexpr std::string_view browser_user_agent = kamaji_user_agent;

  static_assert(api_url.starts_with(legacy_cloud_stream_origin));
  static_assert(store_container_url.starts_with(legacy_cloud_stream_origin));

  /// @brief The session request.
  static constexpr net::http::Endpoint create_session_endpoint{
      .label = kamaji_create_session_label,
      .method = net::http::Method::post,
  };
  /// @brief The stores request.
  static constexpr net::http::Endpoint stores_endpoint{.label =
                                                           kamaji_stores_label};
  /// @brief Container requests, logged with their query.
  static constexpr net::http::Endpoint container_endpoint{
      .label = kamaji_container_label, .log_query = true};
  /// @brief Category requests, logged with their query.
  static constexpr net::http::Endpoint category_endpoint{
      .label = kamaji_category_label, .log_query = true};

  /// @brief The checkout preview.
  static constexpr net::http::Endpoint buynow_preview_endpoint{
      .label = kamaji_buynow_preview_label,
      .method = net::http::Method::post,
  };

  /// @brief The checkout.
  static constexpr net::http::Endpoint buynow_endpoint{
      .label = kamaji_buynow_label,
      .method = net::http::Method::post,
  };

  /// @brief A Kamaji reply's `header.status_code`.
  struct Header;
  /// @brief A reply with only a header.
  struct Acknowledgement;
  /// @brief The session reply's header, country and language.
  struct SessionReply;
  /// @brief The stores reply's header and base URL.
  struct StoresReply;
  /// @brief The preview reply's header and cart.
  struct PreviewReply;
  /// @brief A container or category page's fields.
  struct PageReply {
    /// @brief The `total_results`, raw.
    std::optional<glz::raw_json> total_results;
    /// @brief The `links`, raw.
    json::ps::Tolerant<std::vector<glz::raw_json>> links;
  };

  /// @brief Clamps a JSON integer to `int`.
  ///
  /// @param value The number.
  /// @return It, clamped; 0 for a non-integer.
  static auto count(const json::ps::Number& value) -> int;
  /// @brief Converts a page.
  ///
  /// @param document The page; empty when it was not an object.
  /// @return The page; empty for an empty document.
  static auto page_of(const json::ps::Tolerant<PageReply>& document)
      -> ContainerPage;
  /// @brief Builds the URL of a store container with the offer parameters
  /// every container request carries.
  ///
  /// Each path component is percent-encoded and cut to its capacity:
  /// kamaji_locale_component_capacity for the country and language,
  /// kamaji_title_id_capacity for the container id.
  ///
  /// @param country The store country.
  /// @param language The store language.
  /// @param container_id The container or title id.
  /// @return The URL, ready for further query parameters.
  static auto container_url(std::string_view country, std::string_view language,
                            std::string_view container_id) -> net::http::Query;
  /// @brief Builds the title container request.
  ///
  /// @param country The store country.
  /// @param language The store language.
  /// @param title_id The title.
  /// @return The request.
  static auto title_container_request(std::string_view country,
                                      std::string_view language,
                                      std::string_view title_id)
      -> net::http::Request;
  /// @brief Decodes a 200 reply.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param response The reply.
  /// @return The decoded reply.
  /// @retval StoreError::Reason::refused The status is not 200.
  /// @retval StoreError::Reason::malformed The reply does not decode.
  template <class Reply>
  [[nodiscard]] static auto document(const net::http::Response& response)
      -> Expected<Reply, StoreError> {
    if (net::HttpStatus{response.status()} != net::HttpStatus::ok)
      return unexpected(
          StoreError{StoreError::Reason::refused, response.status()});
    auto parsed = response.document<Reply>();
    if (!parsed)
      return unexpected(
          StoreError{StoreError::Reason::malformed, response.status()});
    return std::move(parsed.value());
  }
  /// @brief Decodes a container or category page.
  ///
  /// @param response The reply.
  /// @return The page.
  /// @retval StoreError The failures of document().
  [[nodiscard]] static auto page_in(const net::http::Response& response)
      -> Expected<ContainerPage, StoreError>;
  /// @brief Reads a checkout preview.
  ///
  /// @param response The reply.
  /// @return The preview.
  /// @retval StoreError The failures buynow_preview() lists, other than
  ///     failed.
  [[nodiscard]] static auto preview_of(const net::http::Response& response)
      -> Expected<CheckoutPreview, StoreError>;
  /// @brief Reads a reply leniently.
  ///
  /// @tparam Reply The shape the reply is decoded into.
  /// @param response The reply.
  /// @return The decoded reply, or an empty one when the body does not
  ///     decode.
  template <class Reply>
  static auto reply_of(const net::http::Response& response) -> Reply;
  /// @brief Tells whether a Kamaji header reports success.
  ///
  /// @param header The header, if any.
  /// @return True when its status code is kamaji_status_ok.
  static auto accepted(const std::optional<Header>& header) -> bool;

  /// @brief Sends a request.
  ///
  /// @param request The request.
  /// @return The reply, whatever its status.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto send(net::http::Request request)
      -> Expected<net::http::Response, StoreError>;
  /// @brief Sends a GET on the session.
  ///
  /// @param endpoint The endpoint.
  /// @param url The URL.
  /// @return The reply, whatever its status.
  /// @retval StoreError::Reason::failed The request did not complete.
  [[nodiscard]] auto session_get(const net::http::Endpoint& endpoint,
                                 net::http::Query url)
      -> Expected<net::http::Response, StoreError>;
  /// @brief Builds a checkout request with the token and, when held, the
  /// session cookie.
  ///
  /// @param endpoint The endpoint.
  /// @param url The URL.
  /// @param bearer The checkout token, whose label the request is recorded
  ///     under.
  /// @return The request.
  auto checkout_request(const net::http::Endpoint& endpoint,
                        std::string_view url,
                        const auth::BearerToken& bearer) const
      -> net::http::Request;
  /// @brief Keeps the first `JSESSIONID` a reply sets, when it is at most
  /// kamaji_session_cookie_capacity bytes; otherwise the session is
  /// unchanged.
  ///
  /// @param response The reply.
  void adopt_session(const net::http::Response& response);

  /// @brief The client the requests go through.
  net::http::Client client_;
  /// @brief The session cookie; empty until a session is opened.
  auth::SessionCookie session_{SecretString{}, kamaji_session_credential};
};

/// @brief A container page started by KamajiClient::start_container_page(),
/// collected once with wait().
///
/// Move-only. Dropping it uncollected abandons the request.
class KamajiClient::PendingPage {
 public:
  /// @brief Waits for the page and decodes it.
  ///
  /// @return The page.
  /// @retval StoreError::Reason::failed The request did not complete.
  /// @retval StoreError The failures of document().
  [[nodiscard]] auto wait() && -> Expected<ContainerPage, StoreError>;

 private:
  /// @brief Starts pages.
  friend class KamajiClient;

  /// @brief Wraps a started request.
  ///
  /// @param sent The request.
  explicit PendingPage(net::http::Client::Pending sent) noexcept;

  /// @brief The started request.
  net::http::Client::Pending sent_;
};

}  // namespace libchiaking::psn::rest
