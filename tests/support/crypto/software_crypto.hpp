#pragma once

#include "libchiaking/platform/crypto/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace test_support {

class SoftwareCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using Error = libchiaking::platform::CryptoError;
  using Input = std::span<const std::byte>;
  using Output = std::span<std::byte>;
  template <class Value>
  using Outcome = libchiaking::Expected<Value, Error>;

  SoftwareCrypto();
  ~SoftwareCrypto() override;
  SoftwareCrypto(const SoftwareCrypto&) = delete;
  auto operator=(const SoftwareCrypto&) -> SoftwareCrypto& = delete;

  [[nodiscard]] auto sha256(Input input, Output digest)
      -> Outcome<void> override;
  [[nodiscard]] auto hmac_sha256(Input key, Input input, Output digest)
      -> Outcome<void> override;
  [[nodiscard]] auto aes128_encrypt_block(Input key, Input block, Output output)
      -> Outcome<void> override;
  [[nodiscard]] auto aes128_gmac(Input key, Input iv, Input aad, Output tag)
      -> Outcome<void> override;
  [[nodiscard]] auto sha512(Input input, Output digest)
      -> Outcome<void> override;
  [[nodiscard]] auto hmac_sha512(Input key, Input input, Output digest)
      -> Outcome<void> override;
  [[nodiscard]] auto hmac_sha1(Input key, Input input, Output digest)
      -> Outcome<void> override;
  [[nodiscard]] auto scrypt(Input password, Input salt,
                            libchiaking::platform::ScryptParameters parameters,
                            Output output) -> Outcome<void> override;
  [[nodiscard]] auto ed25519_public_key(Input seed, Output public_key)
      -> Outcome<void> override;
  [[nodiscard]] auto ed25519_sign(Input seed, Input message, Output signature)
      -> Outcome<void> override;
  [[nodiscard]] auto ed25519_verify(Input public_key, Input message,
                                    Input signature) -> Outcome<bool> override;
  [[nodiscard]] auto p256_base_multiply(Input scalar, Output point)
      -> Outcome<void> override;
  [[nodiscard]] auto p256_multiply(Input scalar, Input point, Output product)
      -> Outcome<void> override;
  [[nodiscard]] auto p256_add(Input left, Input right, Output sum)
      -> Outcome<void> override;

 private:
  using Bytes = std::span<const std::uint8_t>;

  struct Sha1;
  struct Sha256;
  struct Sha512;
  template <class Core>
  class Digest;
  template <class Core>
  class Hmac;
  class Aes128;
  class Gmac;
  class Scrypt;
  class Field;
  class Edwards;
  class P256;

  static auto view(Input input) noexcept -> Bytes;
  static auto raw(Output output) noexcept -> std::uint8_t*;
  static auto invalid() noexcept -> libchiaking::Unexpected<Error>;

  std::unique_ptr<Edwards> edwards_;
  std::unique_ptr<P256> p256_;
};

}  // namespace test_support
