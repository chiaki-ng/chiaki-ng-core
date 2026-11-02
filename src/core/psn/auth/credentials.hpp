/// @file
/// @brief Brings the users context's credentials and sign-in errors into the
/// PSN authorisation layer, which issues and spends them.
#pragma once

#include "libchiaking/detail/users/credentials.hpp"
#include "libchiaking/detail/users/sign_in.hpp"

namespace libchiaking::psn::auth {

using users::AppRefreshKind;
using users::AppRefreshToken;
using users::AppToken;
using users::AppTokenKind;
using users::AuthCode;
using users::AuthCodeKind;
using users::BearerKind;
using users::BearerToken;
using users::Npsso;
using users::NpssoKind;
using users::RemotePlayRefreshKind;
using users::RemotePlayRefreshToken;
using users::SessionCookie;
using users::SessionCookieKind;
using users::SignInError;
using users::SignInErrorKind;
using users::token_maximum_bytes;

}  // namespace libchiaking::psn::auth
