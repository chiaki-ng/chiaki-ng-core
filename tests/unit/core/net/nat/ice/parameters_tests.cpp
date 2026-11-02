#include "src/core/net/nat/ice/parameters.hpp"
#include "tests/support/net/addresses.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace nat_test {
namespace {

using libchiaking::SecretString;
using libchiaking::net::nat::ice::Candidate;
using libchiaking::net::nat::ice::CandidateType;
using libchiaking::net::nat::ice::Credentials;
using libchiaking::net::nat::ice::Parameters;
using test_support::net::Addresses;

class IceParameterText : public ::testing::Test {
 protected:
  static auto takes_foundation(std::string foundation) -> bool {
    const std::vector<Candidate> candidates{
        Candidate{std::move(foundation),
                  1,
                  {Addresses::ipv4(198, 18, 0, 1), 9000},
                  CandidateType::host,
                  {}}};
    return Parameters::check(candidates).has_value();
  }

  static auto takes_credentials(std::string ufrag, std::string password)
      -> bool {
    return Credentials::make(SecretString{std::move(ufrag)},
                             SecretString{std::move(password)})
        .has_value();
  }

  static auto takes_ufrag(std::string ufrag) -> bool {
    return takes_credentials(std::move(ufrag), std::string(48, 'c'));
  }
};

TEST_F(IceParameterText, TakesFoundationsOfLettersDigitsPlusAndSlash) {
  EXPECT_TRUE(takes_foundation("aZ09+/"));
  EXPECT_TRUE(takes_foundation(std::string(32, 'Q')));
}

TEST_F(IceParameterText, RefusesFoundationsWithOtherCharacters) {
  for (const std::string& refused :
       {std::string{}, std::string{"a-b"}, std::string{"a_b"},
        std::string{"a=b"}, std::string{"a b"}, std::string{"a.b"},
        std::string{"a~b"}, std::string{"\xc3\xa9"}, std::string(33, 'q'),
        std::string{"a\0b", 3}})
    EXPECT_FALSE(takes_foundation(refused)) << refused;
}

TEST_F(IceParameterText, TakesOnlyLowerCaseHexCredentials) {
  EXPECT_TRUE(takes_ufrag("0123456789abcdef"));
  EXPECT_FALSE(takes_ufrag("0123456789ABCDEF"));
  EXPECT_FALSE(takes_ufrag("0123456789abcdeF"));
  EXPECT_FALSE(takes_ufrag("0123456789abcdeg"));
  EXPECT_FALSE(
      takes_credentials("0123456789abcdef", std::string(47, 'c') + "C"));
  EXPECT_TRUE(takes_credentials("0123456789abcdef", std::string(48, 'c')));
}

}  // namespace
}  // namespace nat_test
