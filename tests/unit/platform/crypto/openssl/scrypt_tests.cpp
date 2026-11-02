#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::ScryptParameters;

class OpenSslScrypt : public OpenSslCryptoTest {
 protected:
  [[nodiscard]] auto stretch(ScryptParameters parameters)
      -> libchiaking::Expected<void, libchiaking::platform::CryptoError> {
    return crypto.scrypt(text_bytes("password"), text_bytes("NaCl"), parameters,
                         output);
  }

  std::array<std::byte, 64> output{};
};

TEST_F(OpenSslScrypt, MatchesTheRfc7914PasswordNaClKnownAnswer) {
  ASSERT_TRUE(stretch(ScryptParameters{1024, 8, 16}));
  EXPECT_EQ(hex(output),
            "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
            "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640");
}

TEST_F(OpenSslScrypt, AcceptsTheCostPairingUses) {
  EXPECT_TRUE(stretch(ScryptParameters{32768, 8, 1}));
}

TEST_F(OpenSslScrypt, RefusesACostThatIsNotAPowerOfTwo) {
  EXPECT_FALSE(stretch(ScryptParameters{1000, 8, 1}));
}

TEST_F(OpenSslScrypt, RefusesAZeroBlockSize) {
  EXPECT_FALSE(stretch(ScryptParameters{1024, 0, 1}));
}

TEST_F(OpenSslScrypt, RefusesZeroParallelism) {
  EXPECT_FALSE(stretch(ScryptParameters{1024, 8, 0}));
}

TEST_F(OpenSslScrypt, RefusesAnEmptyOutput) {
  EXPECT_FALSE(crypto.scrypt(text_bytes("password"), text_bytes("NaCl"),
                             ScryptParameters{1024, 8, 1}, {}));
}

}  // namespace
}  // namespace platform_crypto_test
