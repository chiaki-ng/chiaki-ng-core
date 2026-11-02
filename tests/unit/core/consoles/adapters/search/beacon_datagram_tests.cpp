#include "src/core/consoles/model/console/wake_credential.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::Generation;
using libchiaking::consoles::WakeCredential;
using libchiaking::platform::IpAddress;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

static_assert(Beacon::ps4_protocol_version == "00020020");
static_assert(Beacon::ps5_protocol_version == "00030010");
static_assert(Beacon::ps4_port == 987 && Beacon::ps5_port == 9302);
static_assert(Beacon::first_local_port == 9303 &&
              Beacon::last_local_port == 9319);

class BeaconDatagrams : public ::testing::Test {
 protected:
  static constexpr char search_ps5[] =
      "SRCH * HTTP/1.1\n"
      "device-discovery-protocol-version:00030010\n\0";
  static constexpr char search_ps4[] =
      "SRCH * HTTP/1.1\n"
      "device-discovery-protocol-version:00020020\n\0";
  static constexpr char wakeup_ps4[] =
      "WAKEUP * HTTP/1.1\n"
      "client-type:vr\n"
      "auth-type:R\n"
      "model:w\n"
      "app-type:r\n"
      "user-credential:18446744073709551615\n"
      "device-discovery-protocol-version:00020020\n\0";
  static constexpr char wakeup_ps5[] =
      "WAKEUP * HTTP/1.1\n"
      "client-type:vr\n"
      "auth-type:R\n"
      "model:w\n"
      "app-type:r\n"
      "user-credential:0\n"
      "device-discovery-protocol-version:00030010\n\0";

  template <std::size_t Size>
  static auto datagram(const char (&bytes)[Size]) -> std::string {
    return std::string{bytes, Size - 1};
  }

  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  SearchSink sink;
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
};

TEST_F(BeaconDatagrams, SearchMatchesTheReferenceBytes) {
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_search());
  EXPECT_EQ(ps5.receive(), datagram(search_ps5));
  EXPECT_EQ(ps4.receive(), datagram(search_ps4));
}

TEST_F(BeaconDatagrams, WakeupMatchesTheReferenceBytes) {
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_wakeup(IpAddress::loopback_ipv4(), Generation::ps4,
                                 WakeCredential{18446744073709551615ULL}));
  EXPECT_EQ(ps4.receive(), datagram(wakeup_ps4));
  ASSERT_TRUE(beacon.send_wakeup(IpAddress::loopback_ipv4(), Generation::ps5,
                                 WakeCredential{0}));
  EXPECT_EQ(ps5.receive(), datagram(wakeup_ps5));
}

}  // namespace
}  // namespace consoles_test
