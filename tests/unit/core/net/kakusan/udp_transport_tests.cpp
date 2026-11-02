#include "libchiaking/testing/fake_secure_datagram.hpp"
#include "tests/unit/core/net/kakusan/udp_transport_cases.hpp"

namespace net_kakusan_test {
namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(
    FakeSecurity, UdpTransport,
    ::testing::Types<libchiaking::testing::FakeSecureDatagramProvider>);

}  // namespace
}  // namespace net_kakusan_test
