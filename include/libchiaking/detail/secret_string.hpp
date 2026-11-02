/// @file
/// @brief Declares BasicSecret and its aliases SecretString and SecretBytes,
/// variable-length secrets that wipe themselves.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <utility>

#include <libchiaking/detail/secure_zero.hpp>
#include <libchiaking/detail/wiping_allocator.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace libchiaking {

/// @brief A growable, heap-held secret (a token, a password, a key of variable
/// length) whose memory is zeroed before it is released.
///
/// The contents live in a buffer obtained from the allocator. With the default
/// memory::WipingAllocator every buffer is zeroed as it is released: when the
/// secret is destroyed, when it is move-assigned over, and when growth replaces
/// the buffer with a larger one. clear() and a shrinking try_resize() zero the
/// bytes they drop straight away.
///
/// A secret cannot be copied. Moving transfers the buffer without copying the
/// bytes and leaves the source empty. clone() and try_clone() make the only
/// copies, so every duplicate is visible in the code.
///
/// Members come in two flavours. The plain ones (the constructors, clone(),
/// assign(), append(), reserve()) throw `std::bad_alloc` when memory runs out.
/// The try_ ones and from_bytes() report Error::allocation_failed instead and
/// leave the secret unchanged.
///
/// @tparam Element The element type, `char` for text or `std::byte` for binary.
/// @tparam Allocator The allocator that owns the buffer. Replacing the default
///     gives up the wiping unless the replacement does it.
/// @note view(), bytes(), span(), data() and the iterators are plain views into
///     the buffer. They are invalidated by anything that grows, shrinks,
///     assigns or moves the secret, and anything copied out through them is no
///     longer protected.
/// @see SecretString
/// @see SecretBytes
/// @see SecretArray
template <class Element, class Allocator = memory::WipingAllocator<Element>>
class BasicSecret {
  static_assert(std::is_same_v<Element, char> ||
                std::is_same_v<Element, std::byte>);

 public:
  /// @brief The element type.
  using value_type = Element;
  /// @brief Read-only iterator over the elements.
  using const_iterator = const Element*;
  /// @brief Writable reference to one element.
  using reference = Element&;

  /// @brief Why a non-throwing operation failed.
  enum class Error {
    /// @brief Memory for the secret could not be obtained.
    allocation_failed,
  };

  /// @brief Makes a secret holding a copy of some bytes, without throwing.
  /// @param bytes The bytes to copy in. The caller remains responsible for
  ///     wiping the source.
  /// @return The new secret.
  /// @retval Error::allocation_failed Memory could not be obtained.
  static auto from_bytes(std::span<const std::byte> bytes) noexcept
      -> Expected<BasicSecret, Error> {
    try {
      return BasicSecret{bytes};
    } catch (...) {
      return unexpected(Error::allocation_failed);
    }
  }

  /// @brief Makes a byte secret of a given length filled with zeros, ready to
  /// be written into.
  ///
  /// Available only when Element is `std::byte`.
  ///
  /// @param size The length in bytes.
  /// @return The new secret. Throws `std::bad_alloc` when memory runs out.
  static auto zeroed(std::size_t size) -> BasicSecret
    requires std::is_same_v<Element, std::byte>
  {
    BasicSecret secret;
    secret.storage_.resize(size);
    return secret;
  }

  /// @brief Makes an independent copy of the secret, without throwing.
  /// @return The copy.
  /// @retval Error::allocation_failed Memory could not be obtained.
  auto try_clone() const noexcept -> Expected<BasicSecret, Error> {
    return from_bytes(bytes());
  }

  /// @brief Replaces the contents with a copy of some bytes, without
  /// throwing.
  ///
  /// The old buffer is zeroed as it is released.
  ///
  /// @param bytes The new contents.
  /// @return Nothing on success.
  /// @retval Error::allocation_failed Memory could not be obtained; the
  ///     secret is unchanged.
  auto try_assign(std::span<const std::byte> bytes) noexcept
      -> Expected<void, Error> {
    auto next = from_bytes(bytes);
    if (!next) return unexpected(next.error());
    *this = std::move(next).value();
    return {};
  }

  /// @brief Appends a copy of some bytes, without throwing.
  /// @param bytes The bytes to add. They may point into this secret.
  /// @return Nothing on success.
  /// @retval Error::allocation_failed Memory could not be obtained; the
  ///     secret is unchanged.
  auto try_append(std::span<const std::byte> bytes) noexcept
      -> Expected<void, Error> {
    try {
      append(bytes);
      return {};
    } catch (...) {
      return unexpected(Error::allocation_failed);
    }
  }

  /// @brief Changes the length, without throwing.
  ///
  /// Shrinking zeroes the dropped tail before the length changes. Growing
  /// adds zero elements.
  ///
  /// @param size The new length in elements.
  /// @return Nothing on success.
  /// @retval Error::allocation_failed Memory could not be obtained; the
  ///     secret is unchanged.
  auto try_resize(std::size_t size) noexcept -> Expected<void, Error> {
    try {
      resize(size);
      return {};
    } catch (...) {
      return unexpected(Error::allocation_failed);
    }
  }

  /// @brief Changes the length.
  ///
  /// Shrinking zeroes the dropped tail before the length changes. Growing
  /// adds zero elements, and a buffer the secret moves out of is zeroed
  /// before it is released.
  ///
  /// @param size The new length in elements.
  /// @warning Throws `std::bad_alloc` when memory runs out; the secret is
  ///     then unchanged.
  void resize(std::size_t size) {
    if (size < storage_.size())
      memory::secure_zero(
          std::span<Element>{storage_.data() + size, storage_.size() - size});
    storage_.resize(size);
  }

  /// @brief The contents as read-only bytes. Available only when Element is
  /// `std::byte`.
  /// @return A view of every element, valid until the secret grows, shrinks,
  ///     is assigned or is moved; empty when the secret is empty.
  auto span() const noexcept -> std::span<const std::byte>
    requires std::is_same_v<Element, std::byte>
  {
    return storage_;
  }

  /// @brief The contents as writable bytes. Available only when Element is
  /// `std::byte`.
  /// @return A view of every element for writing in place, valid until the
  ///     secret grows, shrinks, is assigned or is moved; empty when the
  ///     secret is empty.
  auto span() noexcept -> std::span<std::byte>
    requires std::is_same_v<Element, std::byte>
  {
    return storage_;
  }

  /// @brief Writable iterator to the first element. Available only when
  /// Element is `std::byte`.
  /// @return The same pointer as data(); equal to end() when the secret is
  ///     empty.
  auto begin() noexcept -> Element*
    requires std::is_same_v<Element, std::byte>
  {
    return storage_.data();
  }

  /// @brief Writable iterator one past the last element. Available only when
  /// Element is `std::byte`.
  /// @return data() plus size(), or data() when the secret is empty; not
  ///     dereferenceable.
  auto end() noexcept -> Element*
    requires std::is_same_v<Element, std::byte>
  {
    return storage_.empty() ? storage_.data()
                            : storage_.data() + storage_.size();
  }

  /// @brief The element at an index.
  /// @param index Index below size(); it is not checked.
  /// @return A reference into this secret.
  auto operator[](std::size_t index) noexcept -> Element& {
    return storage_[index];
  }

  /// @brief The element at an index, read-only.
  /// @param index Index below size(); it is not checked.
  /// @return A reference into this secret.
  auto operator[](std::size_t index) const noexcept -> const Element& {
    return storage_[index];
  }

  /// @brief Makes an empty secret; nothing is allocated.
  BasicSecret() noexcept = default;

  /// @brief Makes a secret holding a copy of some text.
  /// @param text The text to copy in. The caller remains responsible for
  ///     wiping the source. Throws `std::bad_alloc` when memory runs out.
  explicit BasicSecret(std::string_view text) { append(text); }

  /// @brief Makes a secret holding a copy of some bytes.
  /// @param bytes The bytes to copy in. The caller remains responsible for
  ///     wiping the source. Throws `std::bad_alloc` when memory runs out.
  explicit BasicSecret(std::span<const std::byte> bytes) { append(bytes); }

  /// @brief Not copyable; use clone() or try_clone() so each copy of a secret
  /// is explicit.
  BasicSecret(const BasicSecret&) = delete;
  /// @brief Not copy-assignable; use clone() or try_clone() so each copy of a
  /// secret is explicit.
  auto operator=(const BasicSecret&) -> BasicSecret& = delete;
  /// @brief Takes over another secret's buffer without copying the bytes; the
  /// source is left empty.
  BasicSecret(BasicSecret&&) noexcept = default;
  /// @brief Releases the current buffer, which the allocator zeroes, and
  /// takes over another secret's buffer; the source is left empty.
  auto operator=(BasicSecret&&) noexcept -> BasicSecret& = default;
  /// @brief Releases the buffer, which the allocator zeroes first.
  ~BasicSecret() = default;

  /// @brief Makes an independent copy of the secret.
  /// @return The copy. Throws `std::bad_alloc` when memory runs out;
  ///     try_clone() does not throw.
  auto clone() const -> BasicSecret { return BasicSecret{bytes()}; }

  /// @brief Replaces the contents with a copy of some text.
  /// @param text The new contents. The old buffer is zeroed as it is
  ///     released.
  void assign(std::string_view text) { *this = BasicSecret{text}; }

  /// @brief Replaces the contents with a copy of some bytes.
  /// @param bytes The new contents. The old buffer is zeroed as it is
  ///     released.
  void assign(std::span<const std::byte> bytes) { *this = BasicSecret{bytes}; }

  /// @brief Appends a copy of some text.
  /// @param text The text to add.
  void append(std::string_view text) {
    append(std::as_bytes(std::span{text.data(), text.size()}));
  }

  /// @brief Appends a copy of some bytes.
  /// @param bytes The bytes to add. They may point into this secret; that
  ///     case is handled through a temporary wiped copy.
  void append(std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
    if (overlaps(bytes)) {
      append(BasicSecret{bytes}.bytes());
      return;
    }
    const auto* first = reinterpret_cast<const Element*>(bytes.data());
    storage_.insert(storage_.end(), first, first + bytes.size());
  }

  /// @brief Appends one element.
  /// @param value The element to add.
  void push_back(Element value) { storage_.push_back(value); }

  /// @brief Appends a copy of some text.
  /// @param text The text to add.
  /// @return This secret.
  auto operator+=(std::string_view text) -> BasicSecret& {
    append(text);
    return *this;
  }

  /// @brief Appends a copy of another secret's contents.
  /// @param other The secret to copy from; it is left unchanged.
  /// @return This secret.
  auto operator+=(const BasicSecret& other) -> BasicSecret& {
    append(other.bytes());
    return *this;
  }

  /// @brief Appends one element.
  /// @param value The element to add.
  /// @return This secret.
  auto operator+=(Element value) -> BasicSecret& {
    push_back(value);
    return *this;
  }

  /// @brief Obtains room for a number of elements in advance, so that later
  /// appends do not move the secret to a new buffer.
  /// @param capacity The number of elements to make room for.
  void reserve(std::size_t capacity) { storage_.reserve(capacity); }

  /// @brief Zeroes the contents now and makes the secret empty.
  ///
  /// The buffer is kept for reuse; it holds only zeros.
  void clear() noexcept {
    memory::secure_zero(std::span<Element>{storage_});
    storage_.clear();
  }

  /// @brief The length in elements.
  /// @return The number of elements held, which is the byte count for both
  ///     element types; zero after clear().
  auto size() const noexcept -> std::size_t { return storage_.size(); }

  /// @brief Tells whether the secret holds no elements.
  /// @return True when size() is zero, including after clear() and after the
  ///     secret was moved from.
  auto empty() const noexcept -> bool { return storage_.empty(); }

  /// @brief Pointer to the first element, read-only. It may be null when the
  /// secret is empty.
  /// @return A pointer into this secret, valid until its length changes or
  ///     it is assigned or moved.
  auto data() const noexcept -> const Element* { return storage_.data(); }

  /// @brief Pointer to the first element, writable. It may be null when the
  /// secret is empty.
  /// @return A pointer into this secret, valid until its length changes.
  auto data() noexcept -> Element* { return storage_.data(); }

  /// @brief Read-only iterator to the first element.
  /// @return The same pointer as data(); equal to end() when the secret is
  ///     empty.
  auto begin() const noexcept -> const_iterator { return storage_.data(); }

  /// @brief Read-only iterator one past the last element.
  /// @return data() plus size(), or data() when the secret is empty; not
  ///     dereferenceable.
  auto end() const noexcept -> const_iterator {
    return storage_.empty() ? storage_.data()
                            : storage_.data() + storage_.size();
  }

  /// @brief The contents as text, whatever the element type.
  /// @return A view into the secret, not null-terminated; empty when the
  ///     secret is empty.
  auto view() const noexcept -> std::string_view {
    if (storage_.empty()) return {};
    return {reinterpret_cast<const char*>(storage_.data()), storage_.size()};
  }

  /// @brief The contents as read-only bytes, whatever the element type.
  /// @return A span into the secret.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return std::as_bytes(std::span<const Element>{storage_});
  }

 private:
  auto overlaps(std::span<const std::byte> bytes) const noexcept -> bool {
    const auto own = this->bytes();
    if (bytes.empty() || own.empty()) return false;
    const std::less<> before;
    return !before(bytes.data() + bytes.size() - 1, own.data()) &&
           !before(own.data() + own.size() - 1, bytes.data());
  }

  std::vector<Element, Allocator> storage_;
};

/// @brief A wiping, move-only secret made of characters, for tokens and
/// passwords.
using SecretString = BasicSecret<char>;
/// @brief A wiping, move-only secret made of bytes, for keys of variable
/// length.
using SecretBytes = BasicSecret<std::byte>;

}  // namespace libchiaking
