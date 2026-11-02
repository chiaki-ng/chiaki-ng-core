#include "src/core/net/protocol/senkusha/senkusha.hpp"

#include <gtest/gtest.h>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;

TEST(SenkushaUmbrellaHeader, DeclaresAValidDefaultConfiguration) {
  const senkusha::Profile profile;
  const senkusha::Policy policy;
  EXPECT_TRUE(senkusha::Validator::validate_configuration(profile, policy));
}

}  // namespace
}  // namespace senkusha_test
