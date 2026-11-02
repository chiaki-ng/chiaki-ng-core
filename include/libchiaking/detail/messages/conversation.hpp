/// @file
/// @brief Declares Conversation, one of a signed-in user's PSN message
/// conversations, and ChatMessage and ChatThread, what is said in one.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::messages {

/// @brief A point in time to the millisecond, on the system clock.
using MessageTime = std::chrono::sys_time<std::chrono::milliseconds>;

/// @brief One message of a conversation.
///
/// @see Messages::thread
struct ChatMessage {
  /// @brief What the message holds.
  enum class Kind : std::uint8_t {
    /// @brief Text, possibly with an image or a store link the text stands
    /// for.
    text,
    /// @brief Something other than text: a sticker, a voice message, a
    /// shared item or an event such as someone joining.
    other,
  };

  /// @brief PSN's id of the message, which reading before it and marking the
  /// conversation read refer to.
  std::string id;
  /// @brief What the message holds.
  Kind kind{Kind::text};
  /// @brief The text; empty for Kind::other.
  std::string text;
  /// @brief The sender's online id; empty when PSN did not say.
  std::string sender;
  /// @brief The sender's account id in decimal; empty when PSN did not say.
  std::string sender_account_id;
  /// @brief Whether the signed-in user sent it.
  bool from_user{};
  /// @brief When it was sent; the epoch when PSN gave no readable time.
  MessageTime sent{};

  /// @brief Compares every field.
  friend auto operator==(const ChatMessage&, const ChatMessage&)
      -> bool = default;
};

/// @brief One person in a conversation.
struct ChatMember {
  /// @brief The account id in decimal.
  std::string account_id;
  /// @brief The online id; empty when PSN did not say.
  std::string online_id;

  /// @brief Compares every field.
  friend auto operator==(const ChatMember&, const ChatMember&)
      -> bool = default;
};

/// @brief One of the user's message conversations, as PSN lists them.
///
/// @see Messages::conversations
struct Conversation {
  /// @brief PSN's id of the conversation's group.
  std::string group_id;
  /// @brief The thread its messages are read and sent in.
  std::string thread_id;
  /// @brief Whether it is a group of more than two people rather than a
  /// direct conversation.
  bool group{};
  /// @brief The name a group was given; empty for a direct conversation and
  /// an unnamed group.
  std::string name;
  /// @brief The other people in it, without the user.
  std::vector<ChatMember> members;
  /// @brief The newest message, when PSN gave it.
  std::optional<ChatMessage> latest;
  /// @brief When it last changed; the epoch when PSN gave no readable time.
  MessageTime updated{};
  /// @brief Whether it holds messages the user has not read.
  bool unread{};
  /// @brief Whether the user marked it a favourite.
  bool favorite{};

  /// @brief Compares every field.
  friend auto operator==(const Conversation&, const Conversation&)
      -> bool = default;
};

/// @brief A run of a conversation's messages.
///
/// @see Messages::thread
struct ChatThread {
  /// @brief The messages, oldest first.
  std::vector<ChatMessage> messages;
  /// @brief Whether the run reaches the start of the conversation, so there
  /// is nothing older to read.
  bool complete{};

  /// @brief Compares every field.
  friend auto operator==(const ChatThread&, const ChatThread&)
      -> bool = default;
};

}  // namespace libchiaking::messages
