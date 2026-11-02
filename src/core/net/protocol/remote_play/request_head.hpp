/// @file
/// @brief Declares RequestHead, the bounded builder for the HTTP request heads
/// sent to a console's Remote Play service.
///
/// The session, control and registration requests carry secrets (the
/// registration key, encrypted auth values) in their headers, so the head is
/// built in fixed, wiped storage rather than a growing string.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

/// @brief The request head would exceed its capacity; nothing of the piece
/// that did not fit was written.
struct RequestHeadOverflow {};

/// @brief An HTTP/1.1 request head built piece by piece into wiped storage of
/// a fixed size.
///
/// Each append either writes all of its piece or nothing and reports
/// RequestHeadOverflow, so a failed head is never half-written mid-piece.
/// Move-only; the moved-from head becomes empty. Not shared between threads.
///
/// @tparam Capacity The largest head, in bytes.
template <std::size_t Capacity>
class RequestHead {
 public:
  /// @brief Makes an empty head.
  RequestHead() noexcept = default;
  RequestHead(const RequestHead&) = delete;
  RequestHead& operator=(const RequestHead&) = delete;
  /// @brief Takes over another head, leaving it empty.
  ///
  /// @param other The head to take over.
  RequestHead(RequestHead&& other) noexcept
      : bytes_(std::move(other.bytes_)), size_(std::exchange(other.size_, 0)) {}
  /// @brief Takes over another head, leaving it empty and wiping what this
  /// held.
  ///
  /// @param other The head to take over.
  /// @return This head.
  RequestHead& operator=(RequestHead&& other) noexcept {
    bytes_ = std::move(other.bytes_);
    size_ = std::exchange(other.size_, 0);
    return *this;
  }

  /// @brief Whether a host can go into a `Host` header: non-empty and only
  /// visible ASCII (no spaces or control characters).
  ///
  /// @param host The host name or address.
  /// @return True when it is acceptable.
  static constexpr auto accepts_host(std::string_view host) noexcept -> bool {
    return !host.empty() && std::ranges::all_of(host, text::ascii_visible);
  }

  /// @brief Appends the request line `<method> <target> HTTP/1.1` and CRLF.
  ///
  /// @param method The HTTP method, such as "GET".
  /// @param target The request path.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto request_line(std::string_view method,
                                  std::string_view target)
      -> Expected<void, RequestHeadOverflow> {
    return pieces({method, " ", target, " HTTP/1.1\r\n"});
  }

  /// @brief Appends text exactly as given.
  ///
  /// @param raw The text.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto text(std::string_view raw)
      -> Expected<void, RequestHeadOverflow> {
    return pieces({raw});
  }

  /// @brief Appends `<name>: <value>` and CRLF.
  ///
  /// @param name The header name.
  /// @param value The header value, already in wire form.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto header(std::string_view name, std::string_view value)
      -> Expected<void, RequestHeadOverflow> {
    return pieces({name, ": ", value, "\r\n"});
  }

  /// @brief Appends a header whose value is an unsigned number in decimal.
  ///
  /// @tparam Number An unsigned integer type.
  /// @param name The header name.
  /// @param value The number.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  template <std::unsigned_integral Number>
  [[nodiscard]] auto header(std::string_view name, Number value)
      -> Expected<void, RequestHeadOverflow> {
    const Digits digits{value};
    return header(name, digits.view());
  }

  /// @brief Appends `Host: <host>:<port>` and CRLF.
  ///
  /// @param host The host; check it with accepts_host() first.
  /// @param port The port, in decimal.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto host_header(std::string_view host, std::uint16_t port)
      -> Expected<void, RequestHeadOverflow> {
    const Digits digits{port};
    return pieces({"Host: ", host, ":", digits.view(), "\r\n"});
  }

  /// @brief Appends a header whose value is the lowercase hex of some bytes.
  ///
  /// @param name The header name.
  /// @param value The bytes, written as two hex digits each.
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto hex_header(std::string_view name,
                                std::span<const std::byte> value)
      -> Expected<void, RequestHeadOverflow> {
    if (name.size() + value.size() * 2 + 4 > remaining())
      return unexpected(RequestHeadOverflow{});
    static_cast<void>(pieces({name, ": "}));
    Cursor cursor{*this};
    text::append_hex(cursor, value);
    return pieces({"\r\n"});
  }

  /// @brief Appends the blank line that ends the head.
  ///
  /// @return Nothing, or RequestHeadOverflow when it does not fit.
  [[nodiscard]] auto end() -> Expected<void, RequestHeadOverflow> {
    return pieces({"\r\n"});
  }

  /// @brief The head written so far, as text.
  ///
  /// @return A view into this head.
  auto view() const noexcept -> std::string_view {
    return {bytes_.data(), size_};
  }

  /// @brief The head written so far, as bytes to send.
  ///
  /// @return A view into this head.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span{bytes_.data(), size_});
  }

  /// @brief How many bytes have been written.
  ///
  /// @return The size in bytes.
  auto size() const noexcept -> std::size_t { return size_; }

 private:
  /// @brief The decimal text of an unsigned number, in a 20-character buffer
  /// (enough for any 64-bit value).
  class Digits {
   public:
    /// @brief Renders a number.
    ///
    /// @tparam Number An unsigned integer type.
    /// @param value The number.
    template <std::unsigned_integral Number>
    explicit Digits(Number value) noexcept {
      size_ = static_cast<std::size_t>(
          std::to_chars(text_.data(), text_.data() + text_.size(), value).ptr -
          text_.data());
    }

    /// @brief The rendered digits.
    ///
    /// @return A view into this object.
    auto view() const noexcept -> std::string_view {
      return {text_.data(), size_};
    }

   private:
    /// @brief The digit buffer.
    std::array<char, 20> text_{};
    /// @brief How many digits were written.
    std::size_t size_{};
  };

  /// @brief A back-inserter onto the head, so the shared hex encoder can write
  /// into it. The caller must have checked the room first.
  class Cursor {
   public:
    /// @brief Writes into a head.
    ///
    /// @param head The head to append to; must outlive the cursor.
    explicit Cursor(RequestHead& head) noexcept : head_(head) {}

    /// @brief Appends one character, without a bounds check.
    ///
    /// @param value The character.
    void push_back(char value) noexcept { head_.bytes_[head_.size_++] = value; }

   private:
    /// @brief The head being written.
    RequestHead& head_;
  };

  /// @brief How many bytes are still free.
  ///
  /// @return Capacity minus the bytes written.
  auto remaining() const noexcept -> std::size_t { return Capacity - size_; }

  /// @brief Appends several pieces as one: all of them, or none when their
  /// total does not fit.
  ///
  /// @param parts The pieces in order.
  /// @return Nothing, or RequestHeadOverflow.
  [[nodiscard]] auto pieces(std::initializer_list<std::string_view> parts)
      -> Expected<void, RequestHeadOverflow> {
    std::size_t needed = 0;
    for (const auto part : parts) needed += part.size();
    if (needed > remaining()) return unexpected(RequestHeadOverflow{});
    for (const auto part : parts)
      size_ = static_cast<std::size_t>(
          std::ranges::copy(part, bytes_.data() + size_).out - bytes_.data());
    return {};
  }

  /// @brief The head's bytes; only the first size_ are written.
  SecretArray<Capacity, char> bytes_;
  /// @brief How many bytes have been written.
  std::size_t size_{};
};

}  // namespace libchiaking::net::protocol::remote_play
