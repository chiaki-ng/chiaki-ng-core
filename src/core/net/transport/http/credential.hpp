/// @file
/// @brief Brings the users context's Credential into the HTTP layer, which
/// places credentials in requests.
#pragma once

#include "libchiaking/detail/users/credential.hpp"

namespace libchiaking::net::http {

using users::Credential;
using users::CredentialPlacement;

}  // namespace libchiaking::net::http
