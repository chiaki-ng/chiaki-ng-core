/// @file
/// @brief Declares the strict JSON subset peer-link control messages are
/// written in, with one canonical spelling per value.
///
/// The subset has only non-negative integers up to 2^53 - 1, ASCII strings,
/// arrays and objects: no booleans, null, negative or fractional numbers,
/// and no duplicate keys. Nesting and size are tightly bounded, so a peer
/// cannot make the parser work hard. The canonical form sorts object keys
/// and has no white space, so two peers serialising the same message get
/// the same bytes.
#pragma once

#include <libchiaking/detail/secret_string.hpp>

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::json::strict {

/// @brief The largest integer the subset allows, 2^53 - 1, so that every
/// value survives a round trip through an IEEE double.
inline constexpr std::uint64_t max_integer = (std::uint64_t{1} << 53U) - 1;
/// @brief The deepest nesting of objects and arrays parse() accepts.
inline constexpr std::size_t max_depth = 8;
/// @brief The most members in an object, or elements in an array, accepted.
inline constexpr std::size_t max_members = 64;

struct Member;

/// @brief One value of the strict subset: an integer, a string, an array, an
/// object, or a secret string.
///
/// An object's members are kept sorted by key in byte order from
/// construction. A secret is a string whose text is held in a wiping
/// SecretString; it serialises like a string, and parse() never produces
/// one. Move-only: clone() makes the one deliberate deep copy, secrets
/// included. Not synchronised; a const value may be read from several
/// threads.
class Value {
 public:
  /// @brief The elements of an array, in order.
  using Array = std::vector<Value>;
  /// @brief The members of an object, sorted by key.
  using Object = std::vector<Member>;

  /// @brief Makes an integer.
  ///
  /// @param integer The value; should not exceed max_integer, which only
  ///     parse() enforces.
  explicit Value(std::uint64_t integer);
  /// @brief Makes a string.
  ///
  /// @param string The raw text.
  explicit Value(std::string string);
  /// @brief Makes a string from a copy of a view.
  ///
  /// @param string The raw text.
  explicit Value(std::string_view string);
  /// @brief Makes a string from a C string, so that a literal is not taken
  /// for another overload.
  ///
  /// @param string The raw text; must not be null.
  explicit Value(const char* string);
  /// @brief Makes an array.
  ///
  /// @param array The elements.
  explicit Value(Array array);
  /// @brief Makes an object, sorting its members by key.
  ///
  /// @param object The members; keys should be unique, which only parse()
  ///     enforces.
  explicit Value(Object object);
  /// @brief Makes a string whose text stays in wiping memory.
  ///
  /// @param secret The raw text.
  explicit Value(SecretString secret);
  /// @brief Not copyable; use clone().
  Value(const Value&) = delete;
  /// @brief Takes over another value; the other is left valid but
  /// unspecified.
  ///
  /// @param other The value to move from.
  Value(Value&& other) noexcept;
  /// @brief Not copy-assignable; use clone().
  Value& operator=(const Value&) = delete;
  /// @brief Replaces this value with another, moved in.
  ///
  /// @param other The value to move from.
  /// @return This value.
  Value& operator=(Value&& other) noexcept;
  /// @brief Releases the value, wiping any secret.
  ~Value();

  /// @brief Makes a deep copy of the value, each secret into a new wiping
  /// string.
  ///
  /// @return The copy.
  auto clone() const -> Value;

  /// @brief Parses a document of the strict subset.
  ///
  /// Accepts only ASCII without NUL, white space being space, tab, line
  /// feed and carriage return. Strings must not contain raw control
  /// characters and must decode to ASCII. Integers have no sign, leading
  /// zero, fraction or exponent. Keys must be unique. Only white space may
  /// follow the root value.
  ///
  /// @param text The document.
  /// @return The value, objects sorted by key; or nothing when the text is
  ///     outside the subset, nests deeper than max_depth, or has an object
  ///     or array with more than max_members entries.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<Value>;

  /// @brief Serialises a value that holds no secret in canonical form: no
  /// white space, keys in sorted order, minimal string escaping.
  ///
  /// @return The canonical text, or nothing when the value holds a secret
  ///     anywhere, which only canonical_secret() writes out.
  auto canonical() const -> std::optional<std::string>;
  /// @brief Serialises the value in canonical form into wiping memory, for
  /// values that hold secrets.
  ///
  /// @return The canonical text, secrets included.
  auto canonical_secret() const -> SecretString;

  /// @brief Views the integer this value holds.
  ///
  /// @return The integer, or null when the value is not an integer; valid
  ///     while the value lives unchanged.
  auto integer() const noexcept -> const std::uint64_t* {
    return std::get_if<std::uint64_t>(&storage_);
  }
  /// @brief Views the string this value holds.
  ///
  /// @return The string, or null when the value is not a plain string (a
  ///     secret is not one).
  auto string() const noexcept -> const std::string* {
    return std::get_if<std::string>(&storage_);
  }
  /// @brief Views the array this value holds.
  ///
  /// @return The elements, or null when the value is not an array.
  auto array() const noexcept -> const Array* {
    return std::get_if<Array>(&storage_);
  }
  /// @brief Views the object this value holds.
  ///
  /// @return The members sorted by key, or null when the value is not an
  ///     object.
  auto object() const noexcept -> const Object* {
    return std::get_if<Object>(&storage_);
  }
  /// @brief Views the secret string this value holds.
  ///
  /// @return The secret, or null when the value is not a secret.
  auto secret() const noexcept -> const SecretString* {
    return std::get_if<SecretString>(&storage_);
  }

 private:
  /// @brief The recursive-descent parser behind parse().
  class Reader;

  /// @brief Tells whether a byte may appear anywhere in a document.
  ///
  /// @param symbol The byte.
  /// @return True for ASCII other than NUL.
  static auto lexable(char symbol) noexcept -> bool;
  /// @brief Tells whether a decoded string byte is allowed.
  ///
  /// @param symbol The byte.
  /// @return True for ASCII.
  static auto plain(char symbol) noexcept -> bool;
  /// @brief Orders members by key, in byte order.
  ///
  /// @param left One member.
  /// @param right The other member.
  /// @return True when `left`'s key sorts before `right`'s.
  static auto precedes(const Member& left, const Member& right) noexcept
      -> bool;
  /// @brief What a value can hold.
  using Storage =
      std::variant<std::uint64_t, std::string, Array, Object, SecretString>;

  /// @brief Deep-copies storage, cloning every value and secret in it.
  ///
  /// @param storage The storage to copy.
  /// @return The copy.
  static auto duplicate(const Storage& storage) -> Storage;
  /// @brief Tells whether a secret appears anywhere in this value.
  ///
  /// @return True for a secret, or an array or object holding one.
  auto holds_secret() const noexcept -> bool;
  /// @brief Appends an integer in decimal.
  ///
  /// @tparam Output A character container with `append(std::string_view)`.
  /// @param output The container to grow.
  /// @param value The integer.
  template <class Output>
  static void append_integer(Output& output, std::uint64_t value);
  /// @brief Appends this value's canonical form.
  ///
  /// @tparam Output A character container, plain or wiping.
  /// @param output The container to grow.
  template <class Output>
  void append_to(Output& output) const;

  /// @brief The value.
  Storage storage_;
};

/// @brief One member of a strict object: a key and its value.
struct Member {
  /// @brief The member's name, raw (unescaped) ASCII.
  std::string key;
  /// @brief The member's value.
  Value value;
};

/// @brief Reads the members of one strict object by key, tracking which have
/// been read so that unexpected extra members can be refused.
///
/// A decoder asks for every member it knows and then calls finish(): an
/// object with members nobody asked for fails it. Views the object, which
/// must outlive the reader. Not synchronised; one thread at a time.
class ObjectReader {
 public:
  /// @brief Starts reading an object.
  ///
  /// @param object The members; must be sorted by key with no duplicates,
  ///     as Value keeps them.
  /// @return The reader, or nothing when the object has more than
  ///     max_members members or its keys are unsorted or repeated.
  [[nodiscard]] static auto create(std::span<const Member> object) noexcept
      -> std::optional<ObjectReader>;

  /// @brief Reads a plain string member and marks it read.
  ///
  /// @param key The member's name.
  /// @return A view of the string, valid while the object lives; nothing,
  ///     and not marked read, when there is no such member or it is not a
  ///     plain string.
  [[nodiscard]] auto string(std::string_view key) noexcept
      -> std::optional<std::string_view>;
  /// @brief Reads an integer member and marks it read.
  ///
  /// @param key The member's name.
  /// @return The integer; nothing, and not marked read, when there is no
  ///     such member or it is not an integer.
  [[nodiscard]] auto integer(std::string_view key) noexcept
      -> std::optional<std::uint64_t>;
  /// @brief Reads an array member and marks it read.
  ///
  /// @param key The member's name.
  /// @return A view of the elements; nothing, and not marked read, when
  ///     there is no such member or it is not an array.
  [[nodiscard]] auto array(std::string_view key) noexcept
      -> std::optional<std::span<const Value>>;
  /// @brief Reads an object member and marks it read.
  ///
  /// @param key The member's name.
  /// @return A view of the nested members; nothing, and not marked read,
  ///     when there is no such member or it is not an object.
  [[nodiscard]] auto object(std::string_view key) noexcept
      -> std::optional<std::span<const Member>>;
  /// @brief Reads a member of any type and marks it read.
  ///
  /// @param key The member's name.
  /// @return The member's value; nothing when there is no such member.
  [[nodiscard]] auto value(std::string_view key) noexcept
      -> std::optional<std::reference_wrapper<const Value>>;
  /// @brief Tells whether a member is present, without marking it read.
  ///
  /// @param key The member's name.
  /// @return True when the object has a member with that name.
  auto has(std::string_view key) const noexcept -> bool;
  /// @brief Tells whether every member has been read.
  ///
  /// @return True when no member was left unread.
  auto finish() const noexcept -> bool;

 private:
  /// @brief Wraps an object already checked by create().
  ///
  /// @param object The sorted members.
  explicit ObjectReader(std::span<const Member> object) noexcept
      : object_(object) {}
  /// @brief Finds a member by binary search.
  ///
  /// @param key The member's name.
  /// @return The member's index, or nothing when absent.
  auto find(std::string_view key) const noexcept -> std::optional<std::size_t>;
  /// @brief Finds a member, picks the alternative wanted and marks the
  /// member read when it has it.
  ///
  /// @tparam Found What to return, constructed from the picked alternative.
  /// @tparam Pick A Value accessor returning a pointer, null when the
  ///     value is of another type.
  /// @param key The member's name.
  /// @param pick The accessor.
  /// @return The picked value, or nothing when absent or of another type.
  template <class Found, class Pick>
  auto take(std::string_view key, Pick pick) noexcept -> std::optional<Found>;

  /// @brief The object's members, sorted by key.
  std::span<const Member> object_;
  /// @brief Which members, by index, have been read.
  std::bitset<max_members> seen_{};
};

}  // namespace libchiaking::json::strict
