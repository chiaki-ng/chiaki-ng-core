/// @file
/// @brief Declares PmullGmac, the AES-128-GMAC hash and tag arithmetic on the
/// ARMv8 polynomial multiply instructions, apart from the block cipher.
#pragma once

#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::platform::armv8 {

/// @brief GHASH over GF(2^128) with PMULL, and the GMAC steps built on it.
///
/// The object holds the hash subkey H, the encryption of the zero block under
/// the GMAC key, and wipes it when destroyed. It never encrypts anything
/// itself: the caller encrypts the zero block to make H and the counter block
/// from counter_block() to finish the tag, so the class runs wherever the CPU
/// has the ARMv8 cryptographic extension, whatever supplies AES. It holds no
/// mutable state and may be used from several threads at once.
class PmullGmac {
 public:
  /// @brief One 16-byte GHASH block.
  using Block = std::array<std::byte, aes128_block_size>;

  /// @brief Keeps a copy of the hash subkey.
  ///
  /// @param hash_key H, the AES-128 encryption of the zero block under the
  ///     GMAC key.
  explicit PmullGmac(
      std::span<const std::byte, aes128_block_size> hash_key) noexcept;

  /// @brief Builds the pre-counter block J0 for a nonce.
  ///
  /// A 12-byte nonce is followed by a 32-bit counter of one; any other length
  /// is hashed with its bit length, as GCM specifies.
  ///
  /// @param iv The nonce, of any length.
  /// @return J0, for the caller to encrypt under the GMAC key.
  [[nodiscard]] auto counter_block(std::span<const std::byte> iv) const noexcept
      -> Block;

  /// @brief Finishes a tag over data that is authenticated but not encrypted.
  ///
  /// @param aad The authenticated data, of any length.
  /// @param encrypted_counter The encryption of counter_block() for the nonce.
  /// @param tag Receives the leading bytes of the 16-byte tag; at most
  ///     aes128_gcm_tag_size bytes are written.
  void authenticate(
      std::span<const std::byte> aad,
      std::span<const std::byte, aes128_block_size> encrypted_counter,
      std::span<std::byte> tag) const noexcept;

 private:
  /// @brief Multiplies two field elements in GCM's bit order with PMULL.
  ///
  /// @param left The first factor.
  /// @param right The second factor.
  /// @return The product reduced by x^128 + x^7 + x^2 + x + 1.
  static auto multiply(
      const Block& left,
      std::span<const std::byte, aes128_block_size> right) noexcept -> Block;
  /// @brief Builds the final GHASH block of two 64-bit big-endian bit counts.
  ///
  /// @param first_bits The count in the first half.
  /// @param second_bits The count in the second half.
  /// @return The block.
  static auto lengths_block(std::uint64_t first_bits,
                            std::uint64_t second_bits) noexcept -> Block;
  /// @brief Adds up to one block into the state, as if zero-padded.
  ///
  /// @param state The running state.
  /// @param block At most aes128_block_size bytes.
  static void fold(Block& state, std::span<const std::byte> block) noexcept;
  /// @brief Hashes data into the state one zero-padded block at a time.
  ///
  /// @param state The running state.
  /// @param data The bytes to hash, of any length.
  void absorb(Block& state, std::span<const std::byte> data) const noexcept;

  /// @brief The hash subkey H.
  SecretArray<aes128_block_size> hash_key_;
};

}  // namespace libchiaking::platform::armv8
