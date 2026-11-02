#include "libchiaking/platform/dtls/openssl/provider.hpp"
#include "tests/unit/core/net/kakusan/udp_transport_cases.hpp"

namespace net_kakusan_test {
namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(
    OpenSslSecurity, UdpTransport,
    ::testing::Types<
        libchiaking::platform::openssl::OpenSslSecureDatagramProvider>);

}  // namespace
}  // namespace net_kakusan_test
