#pragma once

#include "src/core/utils/text/hex.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform_crypto_test {

class OpenSslCryptoTest : public ::testing::Test {
 protected:
  [[nodiscard]] static auto hex(std::span<const std::byte> bytes)
      -> std::string {
    std::string text;
    libchiaking::text::append_hex(text, bytes);
    return text;
  }

  [[nodiscard]] static auto unhex(std::string_view text)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size() / 2);
    if (!libchiaking::text::decode_hex(text, bytes)) bytes.clear();
    return bytes;
  }

  [[nodiscard]] static auto text_bytes(std::string_view text)
      -> std::vector<std::byte> {
    const auto bytes = std::as_bytes(std::span{text});
    return {bytes.begin(), bytes.end()};
  }

  [[nodiscard]] static auto scalar(std::uint8_t value)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(libchiaking::platform::p256_scalar_size);
    bytes.back() = static_cast<std::byte>(value);
    return bytes;
  }

  libchiaking::platform::openssl::OpenSslCryptoProvider crypto;
};

}  // namespace platform_crypto_test
