/// @file
/// @brief Declares ApiCapture, an opt-in in-memory recorder of the library's
/// HTTP traffic for troubleshooting.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/http/provider.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::diagnostics {

/// @brief Bounded buffer of HTTP request and response snapshots, for a
/// developer diagnosing PlayStation Network calls.
///
/// Capture is off until enable() is called. While on, the tracing HTTP provider
/// inside DefaultPlatform calls begin() and finish() around every request on
/// the thread that performs it; nothing is written to disk and no callback runs
/// there. The frontend drains the records from a single consumer with read() or
/// take().
///
/// Each record is one JSON object: a request record with the method, URL,
/// timeout, headers and body, or a response record with the status, final URL,
/// headers, body and duration, or the transport error. Bodies are
/// Base64-encoded. Unless allow_raw() is set, credentials are replaced by
/// redacted before the record is stored.
///
/// @warning Records can still contain personal data, and with allow_raw() they
///     contain live credentials. Sanitise them before saving or sharing.
class ApiCapture {
 public:
  /// @brief Text type for captured records; its memory is wiped when
  /// released.
  using Text = SecretString;

  /// @brief Largest body captured, in bytes. A larger body is noted as
  /// omitted.
  static constexpr std::size_t body_limit = 16 * bytes_per_kib * bytes_per_kib;
  /// @brief Total size of queued records, in bytes, beyond which new records
  /// are dropped.
  static constexpr std::size_t byte_limit = 48 * bytes_per_kib * bytes_per_kib;
  /// @brief Largest single text field captured, in bytes. A longer one is
  /// replaced by an omission note.
  static constexpr std::size_t field_limit = 128 * bytes_per_kib;
  /// @brief Most headers captured per message. The rest are counted as
  /// omitted.
  static constexpr std::size_t header_count_limit = 256;
  /// @brief Most header name and value bytes captured per message.
  static constexpr std::size_t header_bytes_limit = 65536;
  /// @brief Number of records the buffer holds before new ones are dropped.
  static constexpr std::size_t capacity = 32;
  /// @brief Placeholder written in place of a removed credential.
  static constexpr std::string_view redacted = "[redacted]";

  /// @brief Turns capture on or off. Safe to call from any thread.
  ///
  /// @param enabled True to record requests that begin from now on. Turning
  ///     capture off does not discard records already queued.
  void enable(bool enabled) noexcept;
  /// @brief Chooses whether records keep credentials. Safe to call from any
  /// thread.
  ///
  /// @param allowed True to store URLs, headers and bodies unredacted. False,
  ///     the default, to redact them.
  void allow_raw(bool allowed) noexcept;

  /// @brief Records an outgoing request.
  ///
  /// Called by the tracing HTTP provider just before the request is sent.
  ///
  /// @param request Request about to be performed. Copied into the record;
  ///     not retained.
  /// @param attempt Identifier of the library instance making the call, as
  ///     given by LogQueue::attempt.
  /// @return An identifier to pass to finish(), or 0 when capture is
  ///     disabled.
  auto begin(const platform::HttpRequest& request,
             std::uint64_t attempt) noexcept -> std::uint64_t;
  /// @brief Records the outcome of a request.
  ///
  /// @param attempt Same value that was passed to begin().
  /// @param id Value begin() returned. With 0 the call does nothing.
  /// @param response The response, or the transport error.
  /// @param duration Time the request took.
  void finish(
      std::uint64_t attempt, std::uint64_t id,
      const Expected<platform::HttpResponse, platform::HttpError>& response,
      std::chrono::microseconds duration) noexcept;

  /// @brief Copies the oldest record into a caller's buffer, or reports the
  /// size needed.
  ///
  /// The record is removed only when it fits. Passing an empty span therefore
  /// asks for the size without consuming anything. When no record is queued
  /// but some were dropped, a short record stating the number dropped is
  /// produced instead.
  ///
  /// @param destination Buffer to receive the record's text. No terminator is
  ///     written.
  /// @return Size of the record in bytes, whether or not it was copied; the
  ///     caller compares it with the buffer size to tell which. 0 when there
  ///     is nothing to read or the buffer is momentarily in use by a writer.
  auto read(std::span<char> destination) noexcept -> std::size_t;
  /// @brief Removes and returns the oldest record.
  ///
  /// @return The record, or a record stating how many were dropped when none
  ///     is queued, or std::nullopt when there is nothing to report or the
  ///     buffer is momentarily in use by a writer.
  auto take() -> std::optional<Text>;

  /// @brief Removes credentials from a URL's query string and fragment.
  ///
  /// @param url URL to clean.
  /// @return The URL with the values of sensitive parameters, such as code,
  ///     token and password fields, replaced by redacted. The path is
  ///     unchanged.
  static auto redact_url(std::string_view url) -> Text;
  /// @brief Removes credentials from form-encoded key and value pairs.
  ///
  /// @param text Pairs separated by an ampersand, optionally introduced by a
  ///     question mark or hash.
  /// @return The text with the values of sensitive keys replaced by redacted.
  static auto redact_params(std::string_view text) -> Text;
  /// @brief Removes credentials from a JSON document.
  ///
  /// The text is parsed into a document, every member is visited at any
  /// depth, and the document is written back compactly. Does not block and
  /// keeps no state, so it is safe to call from any thread.
  ///
  /// @param text JSON to clean.
  /// @return The document with every string value whose key contains
  ///     "token", "npsso", "code", "secret", "password" or "key", in any
  ///     letter case, replaced by redacted. Text that does not parse as JSON
  ///     is replaced as a whole by redacted, since its credentials cannot be
  ///     located.
  /// @note Only string values are replaced. A number, boolean, null, object
  ///     or array under a sensitive key is left as it is, so counts and
  ///     identifiers stay readable.
  static auto redact_json(std::string_view text) -> Text;

 private:
  class Turn;

  template <class Make>
  void capture(Make make) noexcept;

  struct Header;
  struct Payload;
  struct RequestRecord;
  struct ResponseRecord;
  struct LossRecord;

  static auto redact_body(std::string_view body) -> Text;
  static auto shown(std::string_view value) -> Text;
  static auto payload(const std::vector<platform::HttpHeader>& headers,
                      const SecretBytes& body, bool raw) -> Payload;
  template <class Record>
  static auto written(const Record& record) -> Text;
  static auto loss(std::uint64_t lost) -> Text;

  std::atomic<bool> enabled_{false};
  std::atomic<bool> raw_{false};
  std::atomic<std::uint64_t> next_{};
  std::atomic<std::uint64_t> dropped_{};
  std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
  std::array<Text, capacity> records_;
  std::size_t head_{};
  std::size_t count_{};
  std::size_t bytes_{};
};

}  // namespace libchiaking::diagnostics
