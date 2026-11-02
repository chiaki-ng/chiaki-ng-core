#include "libchiaking/platform/dtls/openssl/provider.hpp"
#include "tests/unit/core/kakusan/network_end_to_end_cases.hpp"

namespace kakusan_test {
namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(
    OpenSslSecurity, NetworkEndToEnd,
    ::testing::Types<
        libchiaking::platform::openssl::OpenSslSecureDatagramProvider>);

}  // namespace
}  // namespace kakusan_test
