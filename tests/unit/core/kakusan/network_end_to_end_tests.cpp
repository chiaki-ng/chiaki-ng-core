#include "libchiaking/testing/fake_secure_datagram.hpp"
#include "tests/unit/core/kakusan/network_end_to_end_cases.hpp"

namespace kakusan_test {
namespace {

INSTANTIATE_TYPED_TEST_SUITE_P(
    FakeSecurity, NetworkEndToEnd,
    ::testing::Types<libchiaking::testing::FakeSecureDatagramProvider>);

}  // namespace
}  // namespace kakusan_test
