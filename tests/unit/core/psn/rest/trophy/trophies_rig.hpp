#pragma once

#include "src/core/psn/rest/trophy/trophies.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include "src/core/psn/auth/credentials.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {

class TrophiesFixture : public ::testing::Test {
 protected:
  using Reply = RestRig::Reply;

  static constexpr std::string_view token = "SYNTHETIC_TROPHY_TOKEN";
  static constexpr std::string_view base =
      "https://m.np.playstation.com/api/trophy/v1";

  static auto url(std::string_view path) -> std::string {
    return std::string{base} + std::string{path};
  }

  auto authorization() const -> std::string {
    return rig.fake.seen.back().header("Authorization");
  }

  RestRig rig;
  libchiaking::psn::rest::Trophies trophies{
      rig.http, libchiaking::psn::auth::BearerToken{token}};
};

}  // namespace psn_test
