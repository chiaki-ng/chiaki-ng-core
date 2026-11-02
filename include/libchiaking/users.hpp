/// @file
/// @brief Umbrella header for the users context: PsUser, SignedInUsers,
/// RequestBudget and every value type they use.
#pragma once

namespace libchiaking::users {

/// @brief The users the library knows; signs them in and out. Get it from
/// ChiakiNg::users.
class SignedInUsers;
/// @brief One person using the library: a signed-in PSN account or a home user.
class PsUser;
/// @brief A user's public PSN profile.
struct Profile;
/// @brief Why a question put to PSN on a user's behalf got no answer.
struct Unanswered;
/// @brief How fast each PSN account may send requests, shared by every service.
class RequestBudget;

}  // namespace libchiaking::users

#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/users/request_budget.hpp"
#include "libchiaking/detail/users/signed_in_users.hpp"
#include "libchiaking/users_types.hpp"
