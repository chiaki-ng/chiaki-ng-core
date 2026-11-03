/// @file
/// @brief Declares rest::Error, the failure of one PSN REST call, the reasons
/// a call may list in it, and the helpers that turn an HTTP exchange into a
/// reply or that failure.
///
/// A REST call can fail in four places, kept apart so callers can react
/// correctly: the request never completed (net::http::Failure), the service
/// answered with an error status (ErrorStatus), the library refused to build
/// the request (a bad_request reason), or the reply was unusable (a bad_reply
/// reason). Each call names, as the parameters of its Error, exactly the
/// reasons it can give.
#pragma once

#include "src/core/net/transport/http/failure.hpp"
#include "src/core/net/transport/http/response.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/json/parse_error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

/// @brief Which of the four places a REST call failed in.
enum class Cause : std::uint8_t {
  /// @brief The request did not complete, was cancelled, or its reply was
  /// cut off at the size limit.
  failure,
  /// @brief The service answered with a status outside 2xx.
  status,
  /// @brief The library refused to build the request.
  request,
  /// @brief The reply had a success status but could not be used.
  response,
};

/// @brief The reasons a reply that arrived with a success status could not be
/// used. Each is an empty tag type a call lists in its Error.
namespace bad_reply {

/// @brief The reply exceeded the document size limit.
struct TooLarge {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "too_large";
};
/// @brief The JSON nests deeper than the parser allows.
struct TooDeep {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "too_deep";
};
/// @brief The body is not JSON.
struct InvalidJson {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_json";
};
/// @brief The body is JSON but lacks a required field or has one of the
/// wrong type.
struct InvalidSchema {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_schema";
};
/// @brief A session manager reply named a session id of the wrong form.
struct InvalidSessionId {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_session_id";
};
/// @brief A session manager reply named an account id that is not a number.
struct InvalidAccountId {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_account_id";
};
/// @brief A push notification named an online id OnlineId::parse() refuses.
struct InvalidOnlineId {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_online_id";
};
/// @brief A profile named an avatar URL that is not a safe HTTP(S) URL, or
/// the profile base URL reply named one that is not HTTPS.
struct InvalidUrl {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_url";
};
/// @brief A push notification named a member's device unique id that is not
/// 64 hex digits.
struct InvalidDeviceUid {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_device_uid";
};
/// @brief A push notification's `customData1` is not 32 characters of doubly
/// base64-encoded 16 to 20 bytes.
struct InvalidCustomData1 {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_custom_data1";
};
/// @brief The push server address reply named a host that cannot be put in a
/// WebSocket URL.
struct InvalidFqdn {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::response;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_fqdn";
};

}  // namespace bad_reply

/// @brief The reasons a request was refused before it was sent. Each is an
/// empty tag type a call lists in its Error.
namespace bad_request {

/// @brief The session id to address is empty or of the wrong form.
struct InvalidSessionId {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::request;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_session_id";
};
/// @brief A message or request body could not be built or is out of bounds.
struct InvalidMessage {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::request;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_message";
};
/// @brief A URL the request would carry is not acceptable.
struct InvalidUrl {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::request;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_url";
};
/// @brief An online id the request would carry is not acceptable.
struct InvalidOnlineId {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::request;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_online_id";
};
/// @brief The push server host cannot be put in a WebSocket URL.
struct InvalidFqdn {
  /// @brief Where the reason belongs.
  static constexpr Cause cause = Cause::request;
  /// @brief The reason's name in logs.
  static constexpr std::string_view name = "invalid_fqdn";
};

}  // namespace bad_request

/// @brief A tag type that names one reason a call can give.
template <class Reason>
concept ReasonTag = std::is_empty_v<Reason> && requires {
  { Reason::cause } -> std::convertible_to<Cause>;
  { Reason::name } -> std::convertible_to<std::string_view>;
};

/// @brief An error status a service answered with, and an excerpt of its
/// body for the log. An immutable value.
class ErrorStatus {
 public:
  /// @brief Finds the error status of a reply.
  ///
  /// @param response The reply.
  /// @param redact The credential to hide from the excerpt; may be empty.
  /// @return The status, with an excerpt of a 4xx body; nothing for a 2xx
  ///     reply.
  static auto of(const net::http::Response& response, std::string_view redact)
      -> std::optional<ErrorStatus>;

  /// @brief Reads the status.
  ///
  /// @return The HTTP status.
  auto code() const noexcept -> std::uint16_t { return code_; }
  /// @brief Reads the start of a 4xx reply's body.
  ///
  /// @return Up to 200 bytes of whole UTF-8 characters, control bytes
  ///     replaced by spaces, the credential redacted, trimmed; empty for 5xx
  ///     replies. Valid while this object lives.
  auto excerpt() const noexcept -> std::string_view { return excerpt_; }

 private:
  /// @brief The longest excerpt kept, in bytes.
  static constexpr std::size_t excerpt_bytes = 200;
  /// @brief What the credential is replaced by in an excerpt.
  static constexpr std::string_view redaction = "[redacted]";

  /// @brief Makes a loggable excerpt of a body.
  ///
  /// @param body The body.
  /// @param redact The credential to hide; nothing is hidden when empty.
  /// @return The excerpt excerpt() describes.
  static auto excerpt_of(std::string_view body, std::string_view redact)
      -> std::string;

  /// @brief Keeps a status and its excerpt.
  ///
  /// @param code The status.
  /// @param excerpt The excerpt.
  ErrorStatus(std::uint16_t code, std::string excerpt) noexcept;

  /// @brief The HTTP status.
  std::uint16_t code_;
  /// @brief The body's excerpt; empty except for 4xx.
  std::string excerpt_;
};

/// @brief A reason that is one of a list of reasons.
template <class Reason, class... Reasons>
concept OneOf = (std::same_as<Reason, Reasons> || ...);

/// @brief Exactly one of the reasons a decoding step that is not an HTTP call
/// can give, such as reading a push notification. An immutable value.
///
/// @tparam Reasons The bad_reply or bad_request tags the step can give.
template <ReasonTag... Reasons>
class Fault {
 public:
  /// @brief Holds one reason.
  ///
  /// @tparam Reason One of `Reasons`.
  /// @param reason The reason.
  template <OneOf<Reasons...> Reason>
  explicit Fault(Reason reason) noexcept : reason_(reason) {}
  /// @brief Holds the reason of a step that can give fewer reasons.
  ///
  /// @tparam Fewer The other step's reasons, each one of `Reasons`.
  /// @param other The other step's fault.
  template <OneOf<Reasons...>... Fewer>
    requires(!std::same_as<Fault<Fewer...>, Fault>)
  explicit Fault(const Fault<Fewer...>& other) noexcept
      : reason_(std::visit(
            [](auto reason) -> std::variant<Reasons...> { return reason; },
            other.reason_)) {}

  /// @brief Translates a JSON parse failure.
  ///
  /// @param error The failure.
  /// @return TooLarge, TooDeep, InvalidSchema for an unexpected shape, or
  ///     InvalidJson.
  static auto parsed(json::ParseError error) noexcept -> Fault
    requires OneOf<bad_reply::TooLarge, Reasons...> &&
             OneOf<bad_reply::TooDeep, Reasons...> &&
             OneOf<bad_reply::InvalidJson, Reasons...> &&
             OneOf<bad_reply::InvalidSchema, Reasons...>
  {
    switch (error) {
      case json::ParseError::too_large:
        return Fault{bad_reply::TooLarge{}};
      case json::ParseError::too_deep:
        return Fault{bad_reply::TooDeep{}};
      case json::ParseError::unexpected_shape:
        return Fault{bad_reply::InvalidSchema{}};
      case json::ParseError::invalid_json:
        break;
    }
    return Fault{bad_reply::InvalidJson{}};
  }

  /// @brief Tells whether the fault is one reason.
  ///
  /// @tparam Reason One of `Reasons`.
  /// @return True when it is `Reason`.
  template <OneOf<Reasons...> Reason>
  auto is() const noexcept -> bool {
    return std::holds_alternative<Reason>(reason_);
  }
  /// @brief Names the reason, for the log.
  ///
  /// @return The tag's name.
  auto name() const noexcept -> std::string_view {
    return std::visit(
        []<class Held>(Held) -> std::string_view { return Held::name; },
        reason_);
  }

 private:
  template <ReasonTag...>
  friend class Fault;
  template <ReasonTag...>
  friend class Error;

  /// @brief The reason.
  std::variant<Reasons...> reason_;
};

/// @brief A PSN REST call's failure: exactly one of a transport failure, an
/// error status with an excerpt of its body, or one of the reasons the call
/// lists.
///
/// An immutable value, safe to copy and to share between threads. Its excerpt
/// never holds the credential the request carried: the caller names it and
/// every occurrence is replaced by `[redacted]`.
///
/// @tparam Reasons The bad_reply and bad_request tags the call can give,
///     beside a transport failure and an error status.
template <ReasonTag... Reasons>
class Error {
 public:
  /// @brief Wraps a transport failure.
  ///
  /// @param failure The client's failure.
  explicit Error(net::http::Failure failure) noexcept : cause_(failure) {}
  /// @brief Wraps one of the call's reasons.
  ///
  /// @tparam Reason One of `Reasons`.
  /// @param reason The reason.
  template <OneOf<Reasons...> Reason>
  explicit Error(Reason reason) noexcept : cause_(reason) {}
  /// @brief Wraps the fault of a decoding step whose reasons the call lists.
  ///
  /// @tparam Fewer The step's reasons, each one of `Reasons`.
  /// @param fault The step's fault.
  template <OneOf<Reasons...>... Fewer>
  explicit Error(const Fault<Fewer...>& fault) noexcept
      : cause_(std::visit([](auto reason) -> Variant { return reason; },
                          fault.reason_)) {}
  /// @brief Wraps the failure of a call whose reasons this call lists.
  ///
  /// @tparam Fewer The other call's reasons, each one of `Reasons`.
  /// @param other The other call's failure.
  template <OneOf<Reasons...>... Fewer>
    requires(!std::same_as<Error<Fewer...>, Error>)
  explicit Error(const Error<Fewer...>& other)
      : cause_(std::visit([](const auto& cause) -> Variant { return cause; },
                          other.cause_)) {}

  /// @brief Accepts a reply only if it completed with a 2xx status.
  ///
  /// @param sent The outcome of sending.
  /// @param redact The credential to hide from the excerpt; may be empty.
  /// @return The reply.
  /// @retval Cause::failure The request failed, or the reply was oversized
  ///     (reported as response_too_large).
  /// @retval Cause::status The status is outside 2xx; for 4xx the excerpt
  ///     holds up to 200 bytes of the body.
  [[nodiscard]] static auto checked(
      Expected<net::http::Response, net::http::Failure> sent,
      std::string_view redact) -> Expected<net::http::Response, Error> {
    if (!sent) return unexpected(Error{sent.error()});
    if (auto refused = refusal(sent.value(), redact))
      return unexpected(std::move(*refused));
    return std::move(sent.value());
  }
  /// @brief Translates a JSON parse failure.
  ///
  /// @param error The failure.
  /// @return The reason Fault::parsed() gives.
  static auto parsed(json::ParseError error) noexcept -> Error
    requires OneOf<bad_reply::TooLarge, Reasons...> &&
             OneOf<bad_reply::TooDeep, Reasons...> &&
             OneOf<bad_reply::InvalidJson, Reasons...> &&
             OneOf<bad_reply::InvalidSchema, Reasons...>
  {
    return Error{
        Fault<bad_reply::TooLarge, bad_reply::TooDeep, bad_reply::InvalidJson,
              bad_reply::InvalidSchema>::parsed(error)};
  }
  /// @brief Reads a reply body as PS JSON into a type, whatever its status.
  ///
  /// Defined in error_reply.hpp, which a caller includes to use it.
  ///
  /// @tparam Reply A glaze-readable type.
  /// @param response The reply.
  /// @return The decoded reply.
  /// @retval bad_reply::InvalidSchema The body is well-formed JSON of the
  ///     wrong shape.
  /// @retval bad_reply::InvalidJson The body is not JSON.
  template <class Reply>
  [[nodiscard]] static auto reply_of(const net::http::Response& response)
      -> Expected<Reply, Error>
    requires OneOf<bad_reply::InvalidJson, Reasons...> &&
             OneOf<bad_reply::InvalidSchema, Reasons...>;
  /// @brief Accepts a reply as checked() does, then reads it as reply_of()
  /// does.
  ///
  /// Defined in error_reply.hpp, which a caller includes to use it.
  ///
  /// @tparam Reply A glaze-readable type.
  /// @param response The reply.
  /// @param redact The credential to hide from the excerpt.
  /// @return The decoded reply.
  /// @retval Error The failures of checked() and reply_of().
  template <class Reply>
  [[nodiscard]] static auto checked_reply(const net::http::Response& response,
                                          std::string_view redact)
      -> Expected<Reply, Error>;
  /// @brief Accepts the outcome of sending as checked() does, then reads the
  /// reply as reply_of() does.
  ///
  /// Defined in error_reply.hpp, which a caller includes to use it.
  ///
  /// @tparam Reply A glaze-readable type.
  /// @param sent The outcome of sending.
  /// @param redact The credential to hide from the excerpt.
  /// @return The decoded reply.
  /// @retval Error The failures of checked() and reply_of().
  template <class Reply>
  [[nodiscard]] static auto checked_reply(
      Expected<net::http::Response, net::http::Failure> sent,
      std::string_view redact) -> Expected<Reply, Error>;

  /// @brief Tells which place the call failed in.
  ///
  /// @return The cause; for one of the call's reasons, the tag's cause.
  auto cause() const noexcept -> Cause {
    return std::visit(
        []<class Held>(const Held&) -> Cause {
          if constexpr (std::same_as<Held, net::http::Failure>)
            return Cause::failure;
          else if constexpr (std::same_as<Held, ErrorStatus>)
            return Cause::status;
          else
            return Held::cause;
        },
        cause_);
  }
  /// @brief Tells whether the failure is one of the call's reasons.
  ///
  /// @tparam Reason One of `Reasons`.
  /// @return True when it is `Reason`.
  template <OneOf<Reasons...> Reason>
  auto is() const noexcept -> bool {
    return std::holds_alternative<Reason>(cause_);
  }
  /// @brief Names the call's reason, for the log.
  ///
  /// @return The tag's name; empty for a transport failure or a status.
  auto reason_name() const noexcept -> std::string_view {
    return std::visit(
        []<class Held>(const Held&) -> std::string_view {
          if constexpr (ReasonTag<Held>)
            return Held::name;
          else
            return {};
        },
        cause_);
  }
  /// @brief Reads the transport failure.
  ///
  /// @return It, or nothing for another cause.
  auto failure() const noexcept -> std::optional<net::http::Failure> {
    if (const auto* failure = std::get_if<net::http::Failure>(&cause_))
      return *failure;
    return std::nullopt;
  }
  /// @brief Reads the error status.
  ///
  /// @return It, or 0 for another cause.
  auto http_status() const noexcept -> std::uint16_t {
    const auto* status = std::get_if<ErrorStatus>(&cause_);
    return status == nullptr ? std::uint16_t{} : status->code();
  }
  /// @brief Reads the start of a 4xx reply's body, for the log.
  ///
  /// @return ErrorStatus::excerpt(); empty for other causes. Valid while
  ///     this object lives.
  auto excerpt() const noexcept -> std::string_view {
    const auto* status = std::get_if<ErrorStatus>(&cause_);
    return status == nullptr ? std::string_view{} : status->excerpt();
  }
  /// @brief Tells whether the stop token ended the request.
  ///
  /// @return True for a cancelled transport failure.
  auto is_cancelled() const noexcept -> bool {
    const auto* failure = std::get_if<net::http::Failure>(&cause_);
    return failure != nullptr && failure->is_cancelled();
  }
  /// @brief Tells whether the service refused the credential.
  ///
  /// @return True for HTTP 401 and 403.
  auto auth_refused() const noexcept -> bool {
    return net::HttpStatus{http_status()}.is_auth_refused();
  }
  /// @brief Tells whether the service rejected the request itself.
  ///
  /// @return True for any 4xx status, 401 and 403 included.
  auto rejected() const noexcept -> bool {
    return net::HttpStatus{http_status()}.is_client_error();
  }

 private:
  template <ReasonTag...>
  friend class Error;

  /// @brief Every cause the call can have.
  using Variant = std::variant<net::http::Failure, ErrorStatus, Reasons...>;

  /// @brief Wraps an error status.
  ///
  /// @param status The status and excerpt.
  explicit Error(ErrorStatus status) noexcept : cause_(std::move(status)) {}

  /// @brief Finds what is wrong with a reply that arrived.
  ///
  /// @param response The reply.
  /// @param redact The credential to hide from the excerpt.
  /// @return A transport failure for an oversized reply, a status failure
  ///     for a non-2xx status, or nothing for a usable reply.
  static auto refusal(const net::http::Response& response,
                      std::string_view redact) -> std::optional<Error> {
    if (response.oversized())
      return Error{net::http::Failure::transport(
          platform::HttpError::response_too_large, response.attempts())};
    if (auto status = ErrorStatus::of(response, redact))
      return Error{std::move(*status)};
    return std::nullopt;
  }

  /// @brief The cause.
  Variant cause_;
};

}  // namespace libchiaking::psn::rest
