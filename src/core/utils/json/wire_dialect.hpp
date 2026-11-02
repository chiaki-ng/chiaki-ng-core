/// @file
/// @brief Declares WireDialect, the reader for JSON as consoles and PS's
/// push, session and holepunch services actually send it, which is looser
/// than RFC 8259.
///
/// Such text is first rebuilt into canonical JSON, then read with glaze, so
/// that glaze only ever sees well-formed input. The same tree also yields
/// the concealed and marked forms the diagnostics log shows.
#pragma once

#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::json {

/// @brief The tolerant JSON dialect of PS's services, and the canonical
/// form it is rebuilt into.
///
/// What the dialect accepts beyond RFC 8259, each seen from PS:
/// - a UTF-8 byte order mark before the document;
/// - anything after the root value, and any bytes after a top-level
///   `true`, `false` or `null`, which are ignored;
/// - numbers with leading zeros, such as `004`, read as `4`;
/// - raw control characters, NULs and invalid UTF-8 inside strings, kept as
///   they are;
/// - repeated keys, the last value winning in the first one's place;
/// - a member with no value at all, but only at omitted_value_path, read as
///   an empty object.
///
/// The canonical form is compact JSON: no white space, members in the order
/// received, strings re-escaped with the minimal escaping, number tokens as
/// sent apart from leading zeros. Every intermediate copy is held in wiping
/// memory, as replies carry tokens.
///
/// All members are static and reentrant; the class has no state.
class WireDialect {
 public:
  /// @brief The largest wire text accepted, in bytes.
  static constexpr std::size_t maximum_bytes = max_document_bytes;
  /// @brief The deepest nesting of objects and arrays accepted.
  static constexpr std::size_t maximum_depth = 64;
  /// @brief The JSON Pointer of the one member that may arrive with no
  /// value: the holepunch offer's local peer address, which the console
  /// leaves empty.
  static constexpr std::string_view omitted_value_path =
      "/connRequest/localPeerAddr";

  /// @brief Says, from a member's key, whether its string value is secret.
  using Conceals = std::function<bool(std::string_view key)>;

  /// @brief Pretty-printed text with the spans of its secret string values,
  /// so that a log can show or mask them.
  struct Marked {
    /// @brief The document, indented by indent_width spaces per level.
    SecretString text;
    /// @brief Each concealed string's content as a byte offset into
    /// `text` and a length, excluding the quotes; in document order.
    std::vector<std::pair<std::size_t, std::size_t>> concealed;
  };

  /// @brief Rebuilds wire text as canonical JSON.
  ///
  /// @param wire The text as received.
  /// @return The canonical text.
  /// @retval ParseError::too_large The text exceeds maximum_bytes.
  /// @retval ParseError::too_deep It nests deeper than maximum_depth.
  /// @retval ParseError::invalid_json It is not JSON even in the dialect.
  [[nodiscard]] static auto canonical_text(std::string_view wire)
      -> Expected<SecretString, ParseError>;

  /// @brief Rebuilds wire text as canonical JSON with secret string values
  /// replaced, for capture files that must not hold secrets.
  ///
  /// Only string values are replaced; a concealed key holding a number,
  /// object or array is written as it is.
  ///
  /// @param wire The text as received.
  /// @param conceals Picks the keys whose string values are secret; called
  ///     for every member at every depth.
  /// @param placeholder What each secret value becomes, written as a JSON
  ///     string.
  /// @return The canonical text with secrets replaced.
  /// @retval ParseError Any failure of canonical_text().
  [[nodiscard]] static auto concealed_text(std::string_view wire,
                                           const Conceals& conceals,
                                           std::string_view placeholder)
      -> Expected<SecretString, ParseError>;

  /// @brief Rebuilds wire text as indented JSON, recording where the secret
  /// string values are, for the diagnostics log.
  ///
  /// @param wire The text as received.
  /// @param conceals Picks the keys whose string values are secret.
  /// @return The indented text, secrets in the clear, and their spans.
  /// @retval ParseError Any failure of canonical_text().
  [[nodiscard]] static auto marked_text(std::string_view wire,
                                        const Conceals& conceals)
      -> Expected<Marked, ParseError>;

  /// @brief Reads canonical text into a message type.
  ///
  /// @tparam Message The message type; readable by glaze.
  /// @param canonical Text from canonical_text() or representable_text().
  /// @return The message.
  /// @retval ParseError::unexpected_shape The text does not fit `Message`.
  template <class Message>
  [[nodiscard]] static auto read_canonical(std::string_view canonical)
      -> Expected<Message, ParseError> {
    auto message = ps::read_wire<Message>(canonical);
    if (!message) return unexpected(ParseError::unexpected_shape);
    return std::move(message.value());
  }

  /// @brief Reads wire text into a message type.
  ///
  /// @tparam Message The message type; readable by glaze.
  /// @param wire The text as received.
  /// @return The message.
  /// @retval ParseError Any failure of canonical_text().
  /// @retval ParseError::unexpected_shape The text does not fit `Message`.
  template <class Message>
  [[nodiscard]] static auto read(std::string_view wire)
      -> Expected<Message, ParseError> {
    auto canonical = canonical_text(wire);
    if (!canonical) return unexpected(canonical.error());
    return read_canonical<Message>(canonical.value().view());
  }

  /// @brief Reads wire text into a message type, refusing numbers a double
  /// cannot hold.
  ///
  /// @tparam Message The message type; readable by glaze.
  /// @param wire The text as received.
  /// @return The message.
  /// @retval ParseError Any failure of representable_text().
  /// @retval ParseError::unexpected_shape The text does not fit `Message`.
  template <class Message>
  [[nodiscard]] static auto read_representable(std::string_view wire)
      -> Expected<Message, ParseError> {
    auto canonical = representable_text(wire);
    if (!canonical) return unexpected(canonical.error());
    return read_canonical<Message>(canonical.value().view());
  }

  /// @brief Rebuilds wire text as canonical JSON, refusing numbers whose
  /// value is not finite as a double, such as `1e400`.
  ///
  /// @param wire The text as received.
  /// @return The canonical text.
  /// @retval ParseError::invalid_json A number overflows a double, or the
  ///     text is not JSON even in the dialect.
  /// @retval ParseError Any other failure of canonical_text().
  [[nodiscard]] static auto representable_text(std::string_view wire)
      -> Expected<SecretString, ParseError>;

 private:
  /// @brief What a node of the parsed tree is.
  enum class Kind : std::uint8_t {
    /// @brief An object; its members are in Value's member list.
    object,
    /// @brief An array; its elements are in Value's element list.
    array,
    /// @brief A string; the decoded text is the scalar.
    string,
    /// @brief A number; the token, leading zeros removed, is the scalar.
    number,
    /// @brief `true` or `false`, spelled out in the scalar.
    boolean,
    /// @brief `null`; the scalar is empty.
    null,
    /// @brief A member that arrived with no value, written out as `{}`.
    omitted,
  };

  /// @brief The spaces per nesting level in marked text.
  static constexpr std::size_t indent_width = 2;

  /// @brief Starts a new line indented to a depth in marked text.
  ///
  /// @param output The marked text to grow.
  /// @param depth The nesting level.
  static void append_indent(Marked& output, std::size_t depth);

  /// @brief How canonical text replaces secret string values.
  struct Concealment {
    /// @brief Picks the keys whose string values are secret.
    const Conceals& conceals;
    /// @brief The raw text each secret value becomes.
    std::string_view placeholder;
  };

  class Member;
  /// @brief One node of the parsed tree, holding its scalar text or its
  /// children in wiping memory.
  ///
  /// Move-only. Only the field matching its kind is meaningful.
  class Value {
   public:
    /// @brief An array's elements, in order.
    using Array = std::vector<Value, memory::WipingAllocator<Value>>;
    /// @brief An object's members, in the order first received.
    using Object = std::vector<Member, memory::WipingAllocator<Member>>;

    /// @brief Makes a scalar node.
    ///
    /// @param kind The kind; not object or array.
    /// @param scalar The decoded string, the number token or the boolean
    ///     spelling; empty for null and omitted.
    explicit Value(Kind kind, SecretString scalar = {}) noexcept;
    /// @brief Makes an object node.
    ///
    /// @param members The members.
    explicit Value(Object members) noexcept;
    /// @brief Makes an array node.
    ///
    /// @param elements The elements.
    explicit Value(Array elements) noexcept;
    Value(const Value&) = delete;
    auto operator=(const Value&) -> Value& = delete;
    /// @brief Takes over another node.
    Value(Value&&) noexcept;
    /// @brief Replaces this node with another, moved in.
    ///
    /// @return This node.
    auto operator=(Value&&) noexcept -> Value&;
    /// @brief Releases the node, wiping its text.
    ~Value();

    /// @brief Tells whether every number in this subtree is finite as a
    /// double.
    ///
    /// @return False when some number overflows a double.
    [[nodiscard]] auto representable() const -> bool;
    /// @brief Writes this subtree as canonical JSON.
    ///
    /// @param capacity Bytes to reserve up front, so that the wiping
    ///     string need not grow and leave copies behind.
    /// @param concealment How to replace secret string values, or null
    ///     to write them in the clear.
    /// @return The canonical text.
    auto canonical(std::size_t capacity,
                   const Concealment* concealment = nullptr) const
        -> SecretString;
    /// @brief Appends this subtree as canonical JSON.
    ///
    /// @param output The text to grow.
    /// @param concealment How to replace secret string values, or null.
    void append_canonical(SecretString& output,
                          const Concealment* concealment) const;
    /// @brief Appends this subtree as indented JSON, recording the spans of
    /// secret string values.
    ///
    /// @param output The marked text to grow.
    /// @param conceals Picks the keys whose string values are secret.
    /// @param depth This node's nesting level.
    void append_marked(Marked& output, const Conceals& conceals,
                       std::size_t depth) const;

   private:
    /// @brief What the node is.
    Kind kind_;
    /// @brief The scalar text of a string, number or boolean.
    SecretString scalar_;
    /// @brief An object's members.
    Object members_;
    /// @brief An array's elements.
    Array elements_;
  };

  /// @brief One member of a parsed object: its decoded key and its value,
  /// both in wiping memory. Move-only.
  class Member {
   public:
    /// @brief Makes a member.
    ///
    /// @param name The decoded key.
    /// @param value The value.
    Member(SecretString name, Value value) noexcept;

    /// @brief Returns the key.
    ///
    /// @return The decoded key; valid while the member lives.
    auto name() const noexcept -> std::string_view { return name_.view(); }
    /// @brief Returns the value.
    ///
    /// @return The value.
    auto value() const noexcept -> const Value& { return value_; }
    /// @brief Replaces the value, when a key repeats.
    ///
    /// @param value The later value.
    void replace(Value value) noexcept { value_ = std::move(value); }

   private:
    /// @brief The decoded key.
    SecretString name_;
    /// @brief The value.
    Value value_;
  };

  /// @brief The JSON Pointer of the node being read, built as the reader
  /// descends.
  class Path;
  /// @brief The recursive-descent parser of the dialect.
  class Reader;

  /// @brief Parses wire text into a tree.
  ///
  /// @param wire The text as received.
  /// @return The root node.
  /// @retval ParseError::too_large The text exceeds maximum_bytes.
  /// @retval ParseError::too_deep It nests deeper than maximum_depth.
  /// @retval ParseError::invalid_json It is not JSON even in the dialect.
  [[nodiscard]] static auto tree(std::string_view wire)
      -> Expected<Value, ParseError>;
};

}  // namespace libchiaking::json
