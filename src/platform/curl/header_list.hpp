/// @file
/// @brief Declares HeaderList, the owning, wiping list of HTTP header lines
/// handed to libcurl, since request headers carry bearer tokens and cookies.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/detail/secure_zero.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <curl/curl.h>

#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::platform::curl_detail {

/// @brief Copies text into wiping memory with a NUL terminator, for libcurl
/// options that take C strings.
///
/// @param value The text; should not contain NUL, which would cut it short
///     for libcurl.
/// @return The terminated copy; its data() is a C string.
inline auto terminated(std::string_view value) -> SecretString {
  SecretString result;
  result.reserve(value.size() + 1);
  result.append(value);
  result.push_back('\0');
  return result;
}

/// @brief A `curl_slist` of `Name: value` lines, for `CURLOPT_HTTPHEADER`,
/// that wipes every line before freeing it.
///
/// Lines are built in wiping memory and copied by libcurl into its own
/// allocations, which the list wipes when it is destroyed. Move-only through
/// its unique pointer; must outlive any transfer that uses get(). Not
/// synchronised; one thread at a time.
class HeaderList {
 public:
  /// @brief Why a line could not be added.
  enum class Error { allocation_failed };

  /// @var Error::allocation_failed
  /// @brief Memory for the line, or libcurl's copy of it, could not be
  /// allocated.

  /// @brief Adds a header line at the end, keeping the order given.
  ///
  /// The name and value are not checked here; use acceptable_header()
  /// first.
  ///
  /// @param name The header name.
  /// @param value The header value; may be secret.
  /// @return Nothing on success.
  /// @retval Error::allocation_failed Memory ran out; the list is
  ///     unchanged.
  auto append(std::string_view name, std::string_view value) noexcept
      -> Expected<void, Error> {
    try {
      constexpr std::string_view separator = ": ";
      SecretString line;
      line.reserve(name.size() + separator.size() + value.size() + 1);
      line.append(name);
      line.append(separator);
      line.append(value);
      line.push_back('\0');
      std::unique_ptr<curl_slist, NodeDeleter> node{
          curl_slist_append(nullptr, line.data())};
      if (!node) return unexpected(Error::allocation_failed);
      if (!list_) {
        list_ = std::move(node);
      } else {
        auto* tail = list_.get();
        while (tail->next != nullptr) tail = tail->next;
        tail->next = node.release();
      }
      return {};
    } catch (...) {
      return unexpected(Error::allocation_failed);
    }
  }

  /// @brief Adds a request header at the end.
  ///
  /// @param header The header.
  /// @return Nothing on success.
  /// @retval Error::allocation_failed Memory ran out; the list is
  ///     unchanged.
  auto append(const HttpHeader& header) noexcept -> Expected<void, Error> {
    return append(header.name, header.value.view());
  }

  /// @brief Returns the list for libcurl.
  ///
  /// @return The first node, or null when no line was added; owned by this
  ///     list.
  auto get() const noexcept -> curl_slist* { return list_.get(); }

 private:
  /// @brief Wipes each line of a list, terminator included, then frees the
  /// whole list.
  class NodeDeleter {
   public:
    /// @brief Wipes and frees a list.
    ///
    /// @param head The first node.
    void operator()(curl_slist* head) const noexcept {
      for (auto* node = head; node != nullptr; node = node->next) {
        if (node->data != nullptr)
          memory::secure_zero(
              std::span{node->data, std::strlen(node->data) + 1});
      }
      curl_slist_free_all(head);
    }
  };

  /// @brief The lines, or null when empty.
  std::unique_ptr<curl_slist, NodeDeleter> list_;
};

}  // namespace libchiaking::platform::curl_detail
