/// @file
/// @brief Umbrella header for the messages context: the Messages service, its
/// value types and the users context it depends on.
#pragma once

namespace libchiaking::messages {

/// @brief Reads a signed-in user's PSN conversations and sends text in them.
/// Get it from ChiakiNg::messages.
class Messages;

}  // namespace libchiaking::messages

#include "libchiaking/detail/messages/messages.hpp"
#include "libchiaking/messages_types.hpp"
#include "libchiaking/users.hpp"
