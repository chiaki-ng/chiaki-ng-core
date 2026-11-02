#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::consoles::Host;

static_assert(!std::is_default_constructible_v<Host>);

TEST(Host, KeepsAPrintableName) {
  const auto named = Host::create("ps5.home.example", Generation::ps4);
  ASSERT_TRUE(named);
  EXPECT_EQ(named.value().text(), "ps5.home.example");
  EXPECT_FALSE(named.value().is_ipv6());
  EXPECT_TRUE(named.value().reaches(Generation::ps4));
  EXPECT_TRUE(named.value().reaches(Generation::ps5));
}

TEST(Host, AcceptsAnIpv4AddressAndANameAtTheLimit) {
  EXPECT_TRUE(Host::create("192.168.1.20", Generation::ps4));
  EXPECT_TRUE(Host::create(std::string(Host::limit, 'a'), Generation::ps5));
}

TEST(Host, RefusesAnEmptyName) {
  EXPECT_EQ(Host::create("", Generation::ps5).error(), Host::Invalid::empty);
}

TEST(Host, RefusesANameOverTheLimit) {
  EXPECT_EQ(
      Host::create(std::string(Host::limit + 1, 'a'), Generation::ps5).error(),
      Host::Invalid::too_long);
}

TEST(Host, RefusesAnythingButPrintableAscii) {
  for (const std::string_view refused :
       {std::string_view{"my console"}, std::string_view{"console\n"},
        std::string_view{"a\0b", 3}, std::string_view{"caf\xc3\xa9"}})
    EXPECT_EQ(Host::create(refused, Generation::ps5).error(),
              Host::Invalid::not_printable)
        << refused;
}

TEST(Host, TakesAnIpv6LiteralOnlyForAPs5) {
  for (const std::string_view literal :
       {"fe80::1", "[2001:db8::10]", "fe80::1%3", "::1"}) {
    const auto ps5 = Host::create(literal, Generation::ps5);
    ASSERT_TRUE(ps5) << literal;
    EXPECT_TRUE(ps5.value().is_ipv6()) << literal;
    EXPECT_FALSE(ps5.value().reaches(Generation::ps4)) << literal;
    EXPECT_EQ(Host::create(literal, Generation::ps4).error(),
              Host::Invalid::ipv6_needs_ps5)
        << literal;
  }
}

TEST(Host, EqualsTheSameAddressForEitherGeneration) {
  EXPECT_EQ(Host::create("10.0.0.2", Generation::ps5).value(),
            Host::create("10.0.0.2", Generation::ps4).value());
}

}  // namespace
}  // namespace consoles_test
