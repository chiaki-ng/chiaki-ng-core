#pragma once

#include "src/core/utils/text/hex.hpp"
#include "src/platform/crypto/micro_ecc/key_pair.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>

#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform_crypto_test {

class ScriptedRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  explicit ScriptedRandom(bool fails) noexcept : fails_(fails) {}

  [[nodiscard]] auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    if (fails_)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::io_failure);
    std::ranges::fill(output, std::byte{0});
    return {};
  }

 private:
  bool fails_;
};

class MicroEccKeyPairTest
    : public ::testing::TestWithParam<libchiaking::platform::EcdhCurve> {
 public:
  [[nodiscard]] static auto curve_name(
      const ::testing::TestParamInfo<libchiaking::platform::EcdhCurve>& info)
      -> std::string {
    return std::string{magic_enum::enum_name(info.param)};
  }

 protected:
  using EcdhCurve = libchiaking::platform::EcdhCurve;

  static constexpr std::byte marker{0x5a};

  [[nodiscard]] static auto generate(
      EcdhCurve curve, libchiaking::platform::SecureRandomProvider& random)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::EcdhKeyPair>,
          libchiaking::platform::CryptoError> {
    using libchiaking::platform::micro_ecc::KeyPair;
    switch (curve) {
      case EcdhCurve::secp256k1:
        return KeyPair<EcdhCurve::secp256k1>::generate(random);
      case EcdhCurve::secp256r1:
        return KeyPair<EcdhCurve::secp256r1>::generate(random);
      case EcdhCurve::secp521r1:
        break;
    }
    return KeyPair<EcdhCurve::secp521r1>::generate(random);
  }

  [[nodiscard]] static auto from_hex(std::string_view text)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size() / 2);
    if (!libchiaking::text::decode_hex(text, bytes)) bytes.clear();
    return bytes;
  }

  [[nodiscard]] static auto all_marked(std::span<const std::byte> bytes)
      -> bool {
    return std::ranges::all_of(bytes,
                               [](std::byte byte) { return byte == marker; });
  }

  const EcdhCurve curve = GetParam();
  const std::size_t coordinates_size =
      libchiaking::platform::ecdh_public_coordinates_size(curve);
  const std::size_t secret_size =
      libchiaking::platform::ecdh_shared_secret_size(curve);
  libchiaking::platform::SystemSecureRandom random;
};

}  // namespace platform_crypto_test
