#include "src/core/net/kakusan/digest.hpp"

namespace libchiaking::net::kakusan {

auto Hash::failure(platform::CryptoError error) noexcept -> Error {
  return error == platform::CryptoError::unsupported_capability
             ? Error::crypto_unavailable
             : Error::internal;
}

auto Hash::bytes_of(std::string_view text) noexcept
    -> std::span<const std::byte> {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

auto Hash::sha256(platform::CryptoProvider& crypto,
                  std::span<const std::byte> input) -> Result<Digest256> {
  Digest256 digest{};
  const auto hashed = crypto.sha256(input, digest);
  if (!hashed) return fail(failure(hashed.error()));
  return digest;
}

auto Hash::hmac_sha256(platform::CryptoProvider& crypto,
                       std::span<const std::byte> key,
                       std::span<const std::byte> input) -> Result<Digest256> {
  Digest256 digest{};
  const auto authenticated = crypto.hmac_sha256(key, input, digest);
  if (!authenticated) return fail(failure(authenticated.error()));
  return digest;
}

}  // namespace libchiaking::net::kakusan
