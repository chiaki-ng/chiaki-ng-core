/// @file
/// @brief Declares ConsoleReply, the parsed HTTP head of a reply from a
/// console's Remote Play service.
///
/// Session, control and registration replies are all HTTP/1.1 heads; this
/// wraps picohttpparser so every codec reads status and headers the same way.
#pragma once

#include "libchiaking/detail/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

/// @brief picohttpparser's header record, declared here so the header need
/// not include the parser.
struct phr_header;

namespace libchiaking::net::protocol::remote_play {

/// @brief The status line and header fields of a console's HTTP reply.
///
/// The header names and values are views into the text that was parsed, which
/// must outlive the reply. A value type otherwise; safe to read from any
/// thread once made.
class ConsoleReply {
 public:
  /// @brief One header field, as sent: neither name nor value is case-folded or
  /// trimmed beyond what picohttpparser does.
  struct Header {
    /// @brief The header name.
    std::string_view name;
    /// @brief The header value, without surrounding whitespace.
    std::string_view value;
  };

  /// @brief Why the text could not be read as a reply head.
  enum class Refusal : std::uint8_t {
    /// @brief The head has not been fully received yet: no status line end, or
    /// no blank line ending the headers. Read more and try again.
    incomplete,
    /// @brief The text is not an HTTP/1.1 head, or its status is not a nonzero
    /// number.
    malformed,
  };

  /// @brief Parses a whole reply head: the `HTTP/1.1 <code>` status line and
  /// the header fields up to the blank line.
  ///
  /// @param text The bytes received so far; may hold body bytes after the head.
  /// @return The reply, whose length() counts the status line, the fields and
  ///     the blank line.
  /// @retval Refusal::incomplete The head is not complete yet.
  /// @retval Refusal::malformed The text is not a valid HTTP/1.1 head.
  [[nodiscard]] static auto response(std::string_view text)
      -> Expected<ConsoleReply, Refusal>;
  /// @brief The errors a codec reports for a reply head it could not read
  /// within its size limit.
  ///
  /// @tparam Error The codec's error type.
  template <class Error>
  struct Faults {
    /// @brief The head is longer than the limit, or that many bytes arrived
    /// without one.
    Error too_large;
    /// @brief The text is not a valid HTTP/1.1 head.
    Error invalid;
    /// @brief The head has not been fully received yet.
    Error incomplete;
  };

  /// @brief Parses a whole reply head that must fit within a size limit,
  /// reporting failures as a codec's own errors.
  ///
  /// @tparam Error The codec's error type.
  /// @param text The bytes received so far; may hold body bytes after the head.
  /// @param limit The most bytes the head may take.
  /// @param faults The errors to report.
  /// @return The reply.
  /// @retval Faults::too_large The head is longer than `limit`, or no head
  ///     could be read from `limit` or more bytes.
  /// @retval Faults::invalid The text is not a valid HTTP/1.1 head.
  /// @retval Faults::incomplete The head is not complete yet.
  template <class Error>
  [[nodiscard]] static auto response(std::string_view text, std::size_t limit,
                                     const Faults<Error>& faults)
      -> Expected<ConsoleReply, Error> {
    auto reply = response(text);
    if (!reply) {
      if (text.size() >= limit) return unexpected(faults.too_large);
      return unexpected(reply.error() == Refusal::malformed
                            ? faults.invalid
                            : faults.incomplete);
    }
    if (reply->length() > limit) return unexpected(faults.too_large);
    return std::move(reply).value();
  }
  /// @brief Parses header fields alone, with no status line, up to a blank
  /// line. code() is 0 for the result.
  ///
  /// Used for the body of a registration reply, which is itself a list of
  /// `Name: value` lines.
  ///
  /// @param text The field lines, ending in an empty line.
  /// @return The fields and the number of bytes they took.
  /// @retval Refusal::incomplete The terminating blank line is missing.
  /// @retval Refusal::malformed A line is not a valid header field.
  [[nodiscard]] static auto fields(std::string_view text)
      -> Expected<ConsoleReply, Refusal>;

  /// @brief The status code exactly as parsed.
  ///
  /// @return The code, which may exceed the HTTP range; 0 for fields().
  auto code() const noexcept -> std::int64_t;
  /// @brief The status code as an HTTP status.
  ///
  /// @return The code when it lies in 1..net::HttpStatus::largest_code, else
  ///     0.
  auto status() const noexcept -> std::uint16_t;
  /// @brief How many bytes of the input the head took.
  ///
  /// @return The length including the blank line; any body starts there.
  auto length() const noexcept -> std::size_t;
  /// @brief The header fields.
  ///
  /// @return The fields in the order the console sent them, viewing the
  ///     parsed text.
  auto headers() const noexcept -> std::span<const Header>;

 private:
  /// @brief Makes a reply from its parsed parts.
  ///
  /// @param code The status code, or 0 for a bare field list.
  /// @param length The bytes the head took.
  /// @param headers The fields.
  ConsoleReply(std::int64_t code, std::size_t length,
               std::vector<Header> headers) noexcept;

  /// @brief Reads the code from a status line.
  ///
  /// @param line The status line, with or without its trailing CR.
  /// @return The code.
  /// @retval Refusal::malformed The line does not start with `HTTP/1.1 `, or
  ///     the code is not a nonzero integer.
  static auto status_code(std::string_view line) noexcept
      -> Expected<std::int64_t, Refusal>;

  /// @brief The number of header slots to give the parser: one per two bytes of
  /// input, which no well-formed head can exceed.
  ///
  /// @param text The text to be parsed.
  /// @return The slot count.
  static auto capacity(std::string_view text) noexcept -> std::size_t;
  /// @brief Maps a negative picohttpparser result to a refusal.
  ///
  /// @param consumed The parser's return value; -2 means incomplete.
  /// @return Refusal::incomplete for -2, Refusal::malformed otherwise.
  static auto refusal(int consumed) noexcept -> Refusal;
  /// @brief Turns the parser's records into Header values, dropping the
  /// continuation lines picohttpparser reports without a name.
  ///
  /// @param parsed The records the parser filled.
  /// @return The named fields.
  static auto named(std::span<const phr_header> parsed) -> std::vector<Header>;

  /// @brief The raw status code.
  std::int64_t code_;
  /// @brief The bytes the head took.
  std::size_t length_;
  /// @brief The fields, viewing the parsed text.
  std::vector<Header> headers_;
};

}  // namespace libchiaking::net::protocol::remote_play
