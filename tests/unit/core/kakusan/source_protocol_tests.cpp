#include "libchiaking/detail/play/session_share.hpp"
#include "src/core/net/kakusan/source_setup.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace kakusan_test {
namespace {

using libchiaking::net::kakusan::control::SourceSetup;

TEST(SourceProtocol, IsForwardableExactlyWhenTheShareCarriesItInOneByte) {
  for (std::uint64_t version = 0; version <= 1024; ++version) {
    SCOPED_TRACE(version);
    const bool forwardable =
        version <= UINT8_MAX &&
        libchiaking::play::SessionShare::shares_protocol_version(
            static_cast<std::uint32_t>(version));
    EXPECT_EQ(SourceSetup::supported_protocol(version), forwardable);
  }
}

TEST(SourceProtocol, RefusesTheLargestVersion) {
  EXPECT_FALSE(SourceSetup::supported_protocol(UINT64_MAX));
}

}  // namespace
}  // namespace kakusan_test
