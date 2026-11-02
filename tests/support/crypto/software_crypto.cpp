#include "tests/support/crypto/software_crypto.hpp"

#include "libchiaking/platform/byte_units.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace test_support {

namespace platform = libchiaking::platform;

struct SoftwareCrypto::Sha1 {
  using Word = std::uint32_t;
  static constexpr std::size_t block_size = 64;
  static constexpr std::size_t digest_size = 20;
  static constexpr std::size_t length_size = 8;
  static constexpr std::array<Word, 5> initial{
      0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U, 0xc3d2e1f0U};

  static auto rotate(Word value, unsigned count) noexcept -> Word {
    return (value << count) | (value >> (32U - count));
  }

  static void compress(Word* state, const std::uint8_t* block) noexcept {
    Word schedule[80];
    for (int index = 0; index < 16; ++index)
      schedule[index] = (Word{block[4 * index]} << 24U) |
                        (Word{block[4 * index + 1]} << 16U) |
                        (Word{block[4 * index + 2]} << 8U) |
                        Word{block[4 * index + 3]};
    for (int index = 16; index < 80; ++index)
      schedule[index] = rotate(schedule[index - 3] ^ schedule[index - 8] ^
                                   schedule[index - 14] ^ schedule[index - 16],
                               1);
    Word a = state[0];
    Word b = state[1];
    Word c = state[2];
    Word d = state[3];
    Word e = state[4];
    for (int index = 0; index < 80; ++index) {
      Word mixed = 0;
      Word constant = 0;
      if (index < 20) {
        mixed = (b & c) | (~b & d);
        constant = 0x5a827999U;
      } else if (index < 40) {
        mixed = b ^ c ^ d;
        constant = 0x6ed9eba1U;
      } else if (index < 60) {
        mixed = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdcU;
      } else {
        mixed = b ^ c ^ d;
        constant = 0xca62c1d6U;
      }
      const Word next = rotate(a, 5) + mixed + e + constant + schedule[index];
      e = d;
      d = c;
      c = rotate(b, 30);
      b = a;
      a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
  }
};

struct SoftwareCrypto::Sha256 {
  using Word = std::uint32_t;
  static constexpr std::size_t block_size = 64;
  static constexpr std::size_t digest_size = 32;
  static constexpr std::size_t length_size = 8;
  static constexpr std::array<Word, 8> initial{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  static constexpr Word rounds[64] = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
  };

  static auto rotate(Word value, unsigned count) noexcept -> Word {
    return (value >> count) | (value << (32U - count));
  }

  static void compress(Word* state, const std::uint8_t* block) noexcept {
    Word schedule[64];
    for (int index = 0; index < 16; ++index)
      schedule[index] = (Word{block[4 * index]} << 24U) |
                        (Word{block[4 * index + 1]} << 16U) |
                        (Word{block[4 * index + 2]} << 8U) |
                        Word{block[4 * index + 3]};
    for (int index = 16; index < 64; ++index) {
      const Word early = schedule[index - 15];
      const Word late = schedule[index - 2];
      schedule[index] = schedule[index - 16] +
                        (rotate(early, 7) ^ rotate(early, 18) ^ (early >> 3U)) +
                        schedule[index - 7] +
                        (rotate(late, 17) ^ rotate(late, 19) ^ (late >> 10U));
    }
    Word work[8];
    for (int index = 0; index < 8; ++index) work[index] = state[index];
    for (int index = 0; index < 64; ++index) {
      const Word first =
          work[7] +
          (rotate(work[4], 6) ^ rotate(work[4], 11) ^ rotate(work[4], 25)) +
          ((work[4] & work[5]) ^ (~work[4] & work[6])) + rounds[index] +
          schedule[index];
      const Word second =
          (rotate(work[0], 2) ^ rotate(work[0], 13) ^ rotate(work[0], 22)) +
          ((work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]));
      for (int slot = 7; slot > 0; --slot) work[slot] = work[slot - 1];
      work[4] += first;
      work[0] = first + second;
    }
    for (int index = 0; index < 8; ++index) state[index] += work[index];
  }
};

struct SoftwareCrypto::Sha512 {
  using Word = std::uint64_t;
  static constexpr std::size_t block_size = 128;
  static constexpr std::size_t digest_size = 64;
  static constexpr std::size_t length_size = 16;
  static constexpr std::array<Word, 8> initial{
      0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
      0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
      0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
  };
  static constexpr Word rounds[80] = {
      0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
      0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
      0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
      0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
      0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
      0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
      0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
      0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
      0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
      0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
      0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
      0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
      0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
      0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
      0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
      0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
      0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
      0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
      0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
      0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
      0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
      0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
      0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
      0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
      0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
      0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
      0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
  };

  static auto rotate(Word value, unsigned count) noexcept -> Word {
    return (value >> count) | (value << (64U - count));
  }

  static void compress(Word* state, const std::uint8_t* block) noexcept {
    Word schedule[80];
    for (int index = 0; index < 16; ++index) {
      Word word = 0;
      for (int byte = 0; byte < 8; ++byte)
        word = (word << 8U) | Word{block[8 * index + byte]};
      schedule[index] = word;
    }
    for (int index = 16; index < 80; ++index) {
      const Word early = schedule[index - 15];
      const Word late = schedule[index - 2];
      schedule[index] = schedule[index - 16] +
                        (rotate(early, 1) ^ rotate(early, 8) ^ (early >> 7U)) +
                        schedule[index - 7] +
                        (rotate(late, 19) ^ rotate(late, 61) ^ (late >> 6U));
    }
    Word work[8];
    for (int index = 0; index < 8; ++index) work[index] = state[index];
    for (int index = 0; index < 80; ++index) {
      const Word first =
          work[7] +
          (rotate(work[4], 14) ^ rotate(work[4], 18) ^ rotate(work[4], 41)) +
          ((work[4] & work[5]) ^ (~work[4] & work[6])) + rounds[index] +
          schedule[index];
      const Word second =
          (rotate(work[0], 28) ^ rotate(work[0], 34) ^ rotate(work[0], 39)) +
          ((work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]));
      for (int slot = 7; slot > 0; --slot) work[slot] = work[slot - 1];
      work[4] += first;
      work[0] = first + second;
    }
    for (int index = 0; index < 8; ++index) state[index] += work[index];
  }
};

template <class Core>
class SoftwareCrypto::Digest {
 public:
  static constexpr std::size_t block_size = Core::block_size;
  static constexpr std::size_t digest_size = Core::digest_size;

  static void compute(Bytes input, std::uint8_t* output) noexcept {
    auto state = Core::initial;
    std::size_t offset = 0;
    for (; input.size() - offset >= block_size; offset += block_size)
      Core::compress(state.data(), input.data() + offset);
    std::uint8_t tail[2 * block_size] = {};
    const std::size_t rest = input.size() - offset;
    for (std::size_t index = 0; index < rest; ++index)
      tail[index] = input[offset + index];
    tail[rest] = 0x80;
    const std::size_t total = rest + 1 + Core::length_size <= block_size
                                  ? block_size
                                  : 2 * block_size;
    const std::uint64_t bits = std::uint64_t{input.size()} * 8U;
    for (std::size_t index = 0; index < 8; ++index)
      tail[total - 1 - index] = static_cast<std::uint8_t>(bits >> (8U * index));
    Core::compress(state.data(), tail);
    if (total > block_size) Core::compress(state.data(), tail + block_size);
    constexpr std::size_t width = sizeof(typename Core::Word);
    for (std::size_t index = 0; index < digest_size; ++index)
      output[index] = static_cast<std::uint8_t>(
          state[index / width] >> (8U * (width - 1 - index % width)));
  }
};

template <class Core>
class SoftwareCrypto::Hmac {
 public:
  static void compute(Bytes key, Bytes input, std::uint8_t* output) {
    using Hash = Digest<Core>;
    std::uint8_t pad[Hash::block_size] = {};
    if (key.size() > Hash::block_size)
      Hash::compute(key, pad);
    else
      std::copy(key.begin(), key.end(), pad);
    std::vector<std::uint8_t> inner(Hash::block_size + input.size());
    for (std::size_t index = 0; index < Hash::block_size; ++index)
      inner[index] = pad[index] ^ 0x36U;
    std::copy(input.begin(), input.end(), inner.begin() + Hash::block_size);
    std::uint8_t outer[Hash::block_size + Hash::digest_size];
    for (std::size_t index = 0; index < Hash::block_size; ++index)
      outer[index] = pad[index] ^ 0x5cU;
    Hash::compute(inner, outer + Hash::block_size);
    Hash::compute(outer, output);
  }
};

class SoftwareCrypto::Aes128 {
 public:
  static constexpr std::size_t block_size = 16;

  explicit Aes128(const std::uint8_t* key) noexcept {
    static constexpr std::uint8_t round_constants[10] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    for (std::size_t index = 0; index < block_size; ++index)
      keys_[index] = key[index];
    for (std::size_t index = block_size; index < sizeof(keys_); index += 4) {
      std::uint8_t word[4] = {keys_[index - 4], keys_[index - 3],
                              keys_[index - 2], keys_[index - 1]};
      if (index % block_size == 0) {
        const std::uint8_t first = word[0];
        word[0] =
            substitution[word[1]] ^ round_constants[index / block_size - 1];
        word[1] = substitution[word[2]];
        word[2] = substitution[word[3]];
        word[3] = substitution[first];
      }
      for (std::size_t byte = 0; byte < 4; ++byte)
        keys_[index + byte] = keys_[index - block_size + byte] ^ word[byte];
    }
  }

  void encrypt(const std::uint8_t* block, std::uint8_t* output) const noexcept {
    std::uint8_t state[block_size];
    for (std::size_t index = 0; index < block_size; ++index)
      state[index] = block[index] ^ keys_[index];
    for (std::size_t round = 1; round <= 10; ++round) {
      std::uint8_t shifted[block_size];
      for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t row = 0; row < 4; ++row)
          shifted[4 * column + row] =
              substitution[state[4 * ((column + row) % 4) + row]];
      for (std::size_t column = 0; column < 4; ++column) {
        const std::uint8_t* in = shifted + 4 * column;
        std::uint8_t* out = state + 4 * column;
        if (round == 10) {
          for (std::size_t row = 0; row < 4; ++row) out[row] = in[row];
          continue;
        }
        out[0] = doubled(in[0]) ^ doubled(in[1]) ^ in[1] ^ in[2] ^ in[3];
        out[1] = in[0] ^ doubled(in[1]) ^ doubled(in[2]) ^ in[2] ^ in[3];
        out[2] = in[0] ^ in[1] ^ doubled(in[2]) ^ doubled(in[3]) ^ in[3];
        out[3] = doubled(in[0]) ^ in[0] ^ in[1] ^ in[2] ^ doubled(in[3]);
      }
      for (std::size_t index = 0; index < block_size; ++index)
        state[index] ^= keys_[round * block_size + index];
    }
    for (std::size_t index = 0; index < block_size; ++index)
      output[index] = state[index];
  }

 private:
  static constexpr std::uint8_t substitution[256] = {
      0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
      0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
      0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
      0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
      0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
      0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
      0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
      0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
      0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
      0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
      0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
      0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
      0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
      0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
      0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
      0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
      0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
      0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
      0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
      0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
      0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
      0xb0, 0x54, 0xbb, 0x16,
  };

  static auto doubled(std::uint8_t value) noexcept -> std::uint8_t {
    const unsigned wide = value;
    return static_cast<std::uint8_t>((wide << 1U) ^
                                     ((wide & 0x80U) != 0 ? 0x1bU : 0U));
  }

  std::uint8_t keys_[176] = {};
};

class SoftwareCrypto::Gmac {
 public:
  static constexpr std::size_t tag_size = 16;

  static void compute(const Aes128& cipher, Bytes iv, Bytes aad,
                      std::uint8_t* tag) noexcept {
    const std::uint8_t zero[Aes128::block_size] = {};
    std::uint8_t subkey[Aes128::block_size];
    cipher.encrypt(zero, subkey);
    const Element key = load(subkey);
    std::uint8_t counter[Aes128::block_size] = {};
    if (iv.size() == 12) {
      std::copy(iv.begin(), iv.end(), counter);
      counter[15] = 1;
    } else {
      Element hashed{};
      absorb(hashed, key, iv);
      hashed = multiply(
          Element{hashed.high, hashed.low ^ (std::uint64_t{iv.size()} * 8U)},
          key);
      store(hashed, counter);
    }
    std::uint8_t pad[Aes128::block_size];
    cipher.encrypt(counter, pad);
    Element hashed{};
    absorb(hashed, key, aad);
    hashed = multiply(
        Element{hashed.high ^ (std::uint64_t{aad.size()} * 8U), hashed.low},
        key);
    store(hashed, tag);
    for (std::size_t index = 0; index < tag_size; ++index)
      tag[index] ^= pad[index];
  }

 private:
  struct Element {
    std::uint64_t high{};
    std::uint64_t low{};
  };

  static auto load(const std::uint8_t* bytes) noexcept -> Element {
    Element element;
    for (std::size_t index = 0; index < 8; ++index) {
      element.high = (element.high << 8U) | std::uint64_t{bytes[index]};
      element.low = (element.low << 8U) | std::uint64_t{bytes[8 + index]};
    }
    return element;
  }

  static void store(Element element, std::uint8_t* bytes) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
      bytes[index] =
          static_cast<std::uint8_t>(element.high >> (8U * (7 - index)));
      bytes[8 + index] =
          static_cast<std::uint8_t>(element.low >> (8U * (7 - index)));
    }
  }

  static auto multiply(Element left, Element right) noexcept -> Element {
    Element product;
    for (unsigned bit = 0; bit < 128; ++bit) {
      const std::uint64_t word = bit < 64 ? left.high : left.low;
      if (((word >> (63U - bit % 64U)) & 1U) != 0) {
        product.high ^= right.high;
        product.low ^= right.low;
      }
      const bool carry = (right.low & 1U) != 0;
      right.low = (right.low >> 1U) | (right.high << 63U);
      right.high >>= 1U;
      if (carry) right.high ^= 0xe100000000000000ULL;
    }
    return product;
  }

  static void absorb(Element& hashed, Element key, Bytes data) noexcept {
    for (std::size_t offset = 0; offset < data.size();
         offset += Aes128::block_size) {
      std::uint8_t block[Aes128::block_size] = {};
      const std::size_t count =
          std::min(Aes128::block_size, data.size() - offset);
      for (std::size_t index = 0; index < count; ++index)
        block[index] = data[offset + index];
      const Element loaded = load(block);
      hashed = multiply(
          Element{hashed.high ^ loaded.high, hashed.low ^ loaded.low}, key);
    }
  }
};

class SoftwareCrypto::Scrypt {
 public:
  static constexpr std::uint64_t memory_limit = 64 * libchiaking::bytes_per_mib;

  static auto derive(Bytes password, Bytes salt,
                     platform::ScryptParameters parameters,
                     std::span<std::uint8_t> output) -> bool {
    constexpr std::size_t words_per_block = 16;
    const std::size_t pairs = parameters.block_size;
    const std::size_t words = 2 * pairs * words_per_block;
    const std::uint64_t cost = parameters.cost;
    if (cost * words * sizeof(std::uint32_t) > memory_limit) return false;
    std::vector<std::uint8_t> blocks(parameters.parallelization * words *
                                     sizeof(std::uint32_t));
    expand(password, salt, blocks);
    std::vector<std::uint32_t> table(static_cast<std::size_t>(cost) * words);
    std::vector<std::uint32_t> current(words);
    std::vector<std::uint32_t> scratch(words);
    for (std::size_t lane = 0; lane < parameters.parallelization; ++lane) {
      std::uint8_t* bytes =
          blocks.data() + lane * words * sizeof(std::uint32_t);
      for (std::size_t index = 0; index < words; ++index)
        current[index] = std::uint32_t{bytes[4 * index]} |
                         (std::uint32_t{bytes[4 * index + 1]} << 8U) |
                         (std::uint32_t{bytes[4 * index + 2]} << 16U) |
                         (std::uint32_t{bytes[4 * index + 3]} << 24U);
      for (std::uint64_t step = 0; step < cost; ++step) {
        std::memcpy(table.data() + step * words, current.data(),
                    words * sizeof(std::uint32_t));
        mix(current.data(), scratch.data(), pairs);
      }
      for (std::uint64_t step = 0; step < cost; ++step) {
        const std::uint64_t chosen =
            current[words - words_per_block] & (cost - 1);
        const std::uint32_t* entry = table.data() + chosen * words;
        for (std::size_t index = 0; index < words; ++index)
          current[index] ^= entry[index];
        mix(current.data(), scratch.data(), pairs);
      }
      for (std::size_t index = 0; index < words; ++index)
        for (std::size_t byte = 0; byte < 4; ++byte)
          bytes[4 * index + byte] =
              static_cast<std::uint8_t>(current[index] >> (8U * byte));
    }
    expand(password, blocks, output);
    return true;
  }

 private:
  static void expand(Bytes password, Bytes salt,
                     std::span<std::uint8_t> output) {
    using Mac = Hmac<Sha256>;
    std::vector<std::uint8_t> message(salt.begin(), salt.end());
    message.resize(salt.size() + 4);
    std::uint32_t counter = 1;
    for (std::size_t offset = 0; offset < output.size(); ++counter) {
      for (std::size_t byte = 0; byte < 4; ++byte)
        message[salt.size() + byte] =
            static_cast<std::uint8_t>(counter >> (8U * (3 - byte)));
      std::uint8_t block[Sha256::digest_size];
      Mac::compute(password, message, block);
      const std::size_t count = std::min(sizeof(block), output.size() - offset);
      std::copy(block, block + count,
                output.begin() + static_cast<std::ptrdiff_t>(offset));
      offset += count;
    }
  }

  static auto rotate(std::uint32_t value, unsigned count) noexcept
      -> std::uint32_t {
    return (value << count) | (value >> (32U - count));
  }

  static void permute(std::uint32_t* block) noexcept {
    std::uint32_t x[16];
    for (int index = 0; index < 16; ++index) x[index] = block[index];
    for (int round = 0; round < 4; ++round) {
      x[4] ^= rotate(x[0] + x[12], 7);
      x[8] ^= rotate(x[4] + x[0], 9);
      x[12] ^= rotate(x[8] + x[4], 13);
      x[0] ^= rotate(x[12] + x[8], 18);
      x[9] ^= rotate(x[5] + x[1], 7);
      x[13] ^= rotate(x[9] + x[5], 9);
      x[1] ^= rotate(x[13] + x[9], 13);
      x[5] ^= rotate(x[1] + x[13], 18);
      x[14] ^= rotate(x[10] + x[6], 7);
      x[2] ^= rotate(x[14] + x[10], 9);
      x[6] ^= rotate(x[2] + x[14], 13);
      x[10] ^= rotate(x[6] + x[2], 18);
      x[3] ^= rotate(x[15] + x[11], 7);
      x[7] ^= rotate(x[3] + x[15], 9);
      x[11] ^= rotate(x[7] + x[3], 13);
      x[15] ^= rotate(x[11] + x[7], 18);
      x[1] ^= rotate(x[0] + x[3], 7);
      x[2] ^= rotate(x[1] + x[0], 9);
      x[3] ^= rotate(x[2] + x[1], 13);
      x[0] ^= rotate(x[3] + x[2], 18);
      x[6] ^= rotate(x[5] + x[4], 7);
      x[7] ^= rotate(x[6] + x[5], 9);
      x[4] ^= rotate(x[7] + x[6], 13);
      x[5] ^= rotate(x[4] + x[7], 18);
      x[11] ^= rotate(x[10] + x[9], 7);
      x[8] ^= rotate(x[11] + x[10], 9);
      x[9] ^= rotate(x[8] + x[11], 13);
      x[10] ^= rotate(x[9] + x[8], 18);
      x[12] ^= rotate(x[15] + x[14], 7);
      x[13] ^= rotate(x[12] + x[15], 9);
      x[14] ^= rotate(x[13] + x[12], 13);
      x[15] ^= rotate(x[14] + x[13], 18);
    }
    for (int index = 0; index < 16; ++index) block[index] += x[index];
  }

  static void mix(std::uint32_t* blocks, std::uint32_t* scratch,
                  std::size_t pairs) noexcept {
    std::uint32_t running[16];
    std::memcpy(running, blocks + (2 * pairs - 1) * 16, sizeof(running));
    for (std::size_t index = 0; index < 2 * pairs; ++index) {
      for (int word = 0; word < 16; ++word)
        running[word] ^= blocks[index * 16 + static_cast<std::size_t>(word)];
      permute(running);
      const std::size_t slot = index % 2 == 0 ? index / 2 : pairs + index / 2;
      std::memcpy(scratch + slot * 16, running, sizeof(running));
    }
    std::memcpy(blocks, scratch, 2 * pairs * sizeof(running));
  }
};

class SoftwareCrypto::Field {
 public:
  using Limbs = std::array<std::uint32_t, 8>;
  using Wide = std::array<std::uint32_t, 16>;

  explicit Field(const Limbs& modulus) noexcept : modulus_(modulus) {
    std::uint32_t inverse = modulus[0];
    for (int round = 0; round < 5; ++round)
      inverse *= 2U - modulus[0] * inverse;
    factor_ = 0U - inverse;
    Limbs power{1};
    for (int bit = 0; bit < 512; ++bit) power = add(power, power);
    square_ = power;
    one_ = multiply(Limbs{1}, square_);
  }

  auto modulus() const noexcept -> const Limbs& { return modulus_; }
  auto one() const noexcept -> const Limbs& { return one_; }

  auto add(const Limbs& left, const Limbs& right) const noexcept -> Limbs {
    Limbs sum;
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      carry += std::uint64_t{left[index]} + right[index];
      sum[index] = static_cast<std::uint32_t>(carry);
      carry >>= 32U;
    }
    if (carry != 0 || compare(sum, modulus_) >= 0) lower(sum, modulus_);
    return sum;
  }

  auto subtract(const Limbs& left, const Limbs& right) const noexcept -> Limbs {
    Limbs difference = left;
    if (lower(difference, right)) raise(difference, modulus_);
    return difference;
  }

  auto negate(const Limbs& value) const noexcept -> Limbs {
    return subtract(Limbs{}, value);
  }

  auto multiply(const Limbs& left, const Limbs& right) const noexcept -> Limbs {
    std::uint32_t work[10] = {};
    const std::uint32_t* a = left.data();
    const std::uint32_t* b = right.data();
    const std::uint32_t* m = modulus_.data();
    for (int outer = 0; outer < 8; ++outer) {
      std::uint64_t carry = 0;
      for (int inner = 0; inner < 8; ++inner) {
        carry += work[inner] + std::uint64_t{a[inner]} * b[outer];
        work[inner] = static_cast<std::uint32_t>(carry);
        carry >>= 32U;
      }
      carry += work[8];
      work[8] = static_cast<std::uint32_t>(carry);
      work[9] = static_cast<std::uint32_t>(carry >> 32U);
      const std::uint32_t scale = work[0] * factor_;
      carry = (work[0] + std::uint64_t{scale} * m[0]) >> 32U;
      for (int inner = 1; inner < 8; ++inner) {
        carry += work[inner] + std::uint64_t{scale} * m[inner];
        work[inner - 1] = static_cast<std::uint32_t>(carry);
        carry >>= 32U;
      }
      carry += work[8];
      work[7] = static_cast<std::uint32_t>(carry);
      work[8] = work[9] + static_cast<std::uint32_t>(carry >> 32U);
    }
    Limbs product;
    for (std::size_t index = 0; index < 8; ++index)
      product[index] = work[index];
    if (work[8] != 0 || compare(product, modulus_) >= 0)
      lower(product, modulus_);
    return product;
  }

  auto square(const Limbs& value) const noexcept -> Limbs {
    return multiply(value, value);
  }

  auto power(const Limbs& base, const Limbs& exponent) const noexcept -> Limbs {
    Limbs result = one_;
    for (int index = 255; index >= 0; --index) {
      result = square(result);
      if (bit(exponent, index)) result = multiply(result, base);
    }
    return result;
  }

  auto invert(const Limbs& value) const noexcept -> Limbs {
    Limbs exponent = modulus_;
    lower(exponent, Limbs{2});
    return power(value, exponent);
  }

  auto reduce(Limbs value) const noexcept -> Limbs {
    while (compare(value, modulus_) >= 0) lower(value, modulus_);
    return value;
  }

  auto reduce(const Wide& value) const noexcept -> Limbs {
    Limbs low;
    Limbs high;
    for (std::size_t index = 0; index < 8; ++index) {
      low[index] = value[index];
      high[index] = value[8 + index];
    }
    return add(reduce(low), multiply(reduce(high), square_));
  }

  auto encode(const Limbs& plain) const noexcept -> Limbs {
    return multiply(reduce(plain), square_);
  }

  auto decode(const Limbs& encoded) const noexcept -> Limbs {
    return multiply(encoded, Limbs{1});
  }

  auto small(std::uint32_t value) const noexcept -> Limbs {
    return encode(Limbs{value});
  }

  static auto compare(const Limbs& left, const Limbs& right) noexcept -> int {
    for (std::size_t index = 8; index-- > 0;) {
      if (left[index] != right[index])
        return left[index] < right[index] ? -1 : 1;
    }
    return 0;
  }

  static auto bit(const Limbs& value, int index) noexcept -> bool {
    return ((value[static_cast<std::size_t>(index) / 32] >>
             (static_cast<unsigned>(index) % 32U)) &
            1U) != 0;
  }

  static auto is_zero(const Limbs& value) noexcept -> bool {
    return value == Limbs{};
  }

  static auto big_endian(const std::uint8_t* bytes) noexcept -> Limbs {
    Limbs value{};
    for (std::size_t index = 0; index < 32; ++index)
      value[index / 4] |= std::uint32_t{bytes[31 - index]}
                          << (8U * (index % 4));
    return value;
  }

  static auto little_endian(const std::uint8_t* bytes) noexcept -> Limbs {
    Limbs value{};
    for (std::size_t index = 0; index < 32; ++index)
      value[index / 4] |= std::uint32_t{bytes[index]} << (8U * (index % 4));
    return value;
  }

  static void store_big_endian(const Limbs& value,
                               std::uint8_t* bytes) noexcept {
    for (std::size_t index = 0; index < 32; ++index)
      bytes[31 - index] =
          static_cast<std::uint8_t>(value[index / 4] >> (8U * (index % 4)));
  }

  static void store_little_endian(const Limbs& value,
                                  std::uint8_t* bytes) noexcept {
    for (std::size_t index = 0; index < 32; ++index)
      bytes[index] =
          static_cast<std::uint8_t>(value[index / 4] >> (8U * (index % 4)));
  }

 private:
  static auto lower(Limbs& value, const Limbs& amount) noexcept -> bool {
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      const std::uint64_t difference =
          std::uint64_t{value[index]} - amount[index] - borrow;
      value[index] = static_cast<std::uint32_t>(difference);
      borrow = (difference >> 32U) & 1U;
    }
    return borrow != 0;
  }

  static void raise(Limbs& value, const Limbs& amount) noexcept {
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      carry += std::uint64_t{value[index]} + amount[index];
      value[index] = static_cast<std::uint32_t>(carry);
      carry >>= 32U;
    }
  }

  Limbs modulus_;
  Limbs square_{};
  Limbs one_{};
  std::uint32_t factor_{};
};

class SoftwareCrypto::Edwards {
 public:
  static constexpr std::size_t key_size = 32;
  static constexpr std::size_t signature_size = 64;

  Edwards() noexcept {
    coefficient_ = field_.multiply(field_.negate(field_.small(121665)),
                                   field_.invert(field_.small(121666)));
    doubled_coefficient_ = field_.add(coefficient_, coefficient_);
    root_ =
        field_.power(field_.small(2),
                     Limbs{0xfffffffbU, 0xffffffffU, 0xffffffffU, 0xffffffffU,
                           0xffffffffU, 0xffffffffU, 0xffffffffU, 0x1fffffffU});
    identity_ = Point{Limbs{}, field_.one(), field_.one(), Limbs{}};
    std::uint8_t encoded[key_size];
    Field::store_little_endian(
        field_.decode(
            field_.multiply(field_.small(4), field_.invert(field_.small(5)))),
        encoded);
    base_ = decode(encoded).value_or(identity_);
  }

  void public_key(const std::uint8_t* seed,
                  std::uint8_t* output) const noexcept {
    const auto secret = expand(seed);
    std::copy(secret.key.begin(), secret.key.end(), output);
  }

  void sign(const std::uint8_t* seed, Bytes message,
            std::uint8_t* signature) const {
    const auto secret = expand(seed);
    const Limbs nonce = challenge({Bytes{secret.prefix}, message});
    encode(multiply(nonce, base_), signature);
    const Limbs hashed =
        challenge({Bytes{signature, key_size}, Bytes{secret.key}, message});
    const Limbs response = order_.add(
        nonce, order_.multiply(hashed, order_.encode(secret.scalar)));
    Field::store_little_endian(response, signature + key_size);
  }

  auto verify(const std::uint8_t* key, Bytes message,
              const std::uint8_t* signature) const -> bool {
    const Limbs response = Field::little_endian(signature + key_size);
    if (Field::compare(response, order_.modulus()) >= 0) return false;
    const auto owner = decode(key);
    if (!owner) return false;
    const Limbs hashed =
        challenge({Bytes{signature, key_size}, Bytes{key, key_size}, message});
    const Point expected =
        add(multiply(response, base_), multiply(hashed, negate(*owner)));
    std::uint8_t encoded[key_size];
    encode(expected, encoded);
    return std::equal(encoded, encoded + key_size, signature);
  }

 private:
  using Limbs = Field::Limbs;

  struct Point {
    Limbs x{};
    Limbs y{};
    Limbs z{};
    Limbs t{};
  };

  struct Secret {
    Limbs scalar{};
    std::array<std::uint8_t, key_size> prefix{};
    std::array<std::uint8_t, key_size> key{};
  };

  auto expand(const std::uint8_t* seed) const noexcept -> Secret {
    std::uint8_t hashed[Sha512::digest_size];
    Digest<Sha512>::compute(Bytes{seed, key_size}, hashed);
    hashed[0] &= 248U;
    hashed[31] &= 127U;
    hashed[31] |= 64U;
    Secret secret;
    secret.scalar = Field::little_endian(hashed);
    std::copy(hashed + key_size, hashed + 2 * key_size, secret.prefix.begin());
    encode(multiply(secret.scalar, base_), secret.key.data());
    return secret;
  }

  auto challenge(std::initializer_list<Bytes> parts) const -> Limbs {
    std::vector<std::uint8_t> message;
    for (const auto part : parts)
      message.insert(message.end(), part.begin(), part.end());
    std::uint8_t hashed[Sha512::digest_size];
    Digest<Sha512>::compute(message, hashed);
    Field::Wide wide{};
    for (std::size_t index = 0; index < sizeof(hashed); ++index)
      wide[index / 4] |= std::uint32_t{hashed[index]} << (8U * (index % 4));
    return order_.reduce(wide);
  }

  auto add(const Point& left, const Point& right) const noexcept -> Point {
    const Limbs a = field_.multiply(field_.subtract(left.y, left.x),
                                    field_.subtract(right.y, right.x));
    const Limbs b = field_.multiply(field_.add(left.y, left.x),
                                    field_.add(right.y, right.x));
    const Limbs c =
        field_.multiply(field_.multiply(left.t, doubled_coefficient_), right.t);
    const Limbs zz = field_.multiply(left.z, right.z);
    const Limbs d = field_.add(zz, zz);
    const Limbs e = field_.subtract(b, a);
    const Limbs f = field_.subtract(d, c);
    const Limbs g = field_.add(d, c);
    const Limbs h = field_.add(b, a);
    return Point{field_.multiply(e, f), field_.multiply(g, h),
                 field_.multiply(f, g), field_.multiply(e, h)};
  }

  auto negate(const Point& point) const noexcept -> Point {
    return Point{field_.negate(point.x), point.y, point.z,
                 field_.negate(point.t)};
  }

  auto multiply(const Limbs& scalar, const Point& point) const noexcept
      -> Point {
    Point result = identity_;
    for (int index = 255; index >= 0; --index) {
      result = add(result, result);
      if (Field::bit(scalar, index)) result = add(result, point);
    }
    return result;
  }

  void encode(const Point& point, std::uint8_t* output) const noexcept {
    const Limbs inverse = field_.invert(point.z);
    const Limbs x = field_.decode(field_.multiply(point.x, inverse));
    const Limbs y = field_.decode(field_.multiply(point.y, inverse));
    Field::store_little_endian(y, output);
    output[31] = static_cast<std::uint8_t>(output[31] | ((x[0] & 1U) << 7U));
  }

  auto decode(const std::uint8_t* input) const noexcept
      -> std::optional<Point> {
    const bool odd = (input[31] & 0x80U) != 0;
    Limbs plain = Field::little_endian(input);
    plain[7] &= 0x7fffffffU;
    if (Field::compare(plain, field_.modulus()) >= 0) return std::nullopt;
    const Limbs y = field_.encode(plain);
    const Limbs yy = field_.square(y);
    const Limbs u = field_.subtract(yy, field_.one());
    const Limbs v = field_.add(field_.multiply(coefficient_, yy), field_.one());
    Limbs x =
        field_.power(field_.multiply(u, field_.invert(v)),
                     Limbs{0xfffffffeU, 0xffffffffU, 0xffffffffU, 0xffffffffU,
                           0xffffffffU, 0xffffffffU, 0xffffffffU, 0x0fffffffU});
    const Limbs check = field_.multiply(v, field_.square(x));
    if (check != u) {
      if (check != field_.negate(u)) return std::nullopt;
      x = field_.multiply(x, root_);
    }
    const Limbs affine = field_.decode(x);
    if (Field::is_zero(affine) && odd) return std::nullopt;
    if (((affine[0] & 1U) != 0) != odd) x = field_.negate(x);
    return Point{x, y, field_.one(), field_.multiply(x, y)};
  }

  Field field_{Limbs{0xffffffedU, 0xffffffffU, 0xffffffffU, 0xffffffffU,
                     0xffffffffU, 0xffffffffU, 0xffffffffU, 0x7fffffffU}};
  Field order_{Limbs{0x5cf5d3edU, 0x5812631aU, 0xa2f79cd6U, 0x14def9deU,
                     0x00000000U, 0x00000000U, 0x00000000U, 0x10000000U}};
  Limbs coefficient_{};
  Limbs doubled_coefficient_{};
  Limbs root_{};
  Point identity_;
  Point base_;
};

class SoftwareCrypto::P256 {
 public:
  using Limbs = Field::Limbs;
  static constexpr std::size_t scalar_size = 32;
  static constexpr std::size_t point_size = 65;

  struct Point {
    Limbs x{};
    Limbs y{};
    Limbs z{};
  };

  P256() noexcept
      : curve_(field_.encode(Limbs{0x27d2604bU, 0x3bce3c3eU, 0xcc53b0f6U,
                                   0x651d06b0U, 0x769886bcU, 0xb3ebbd55U,
                                   0xaa3a93e7U, 0x5ac635d8U})),
        base_{field_.encode(Limbs{0xd898c296U, 0xf4a13945U, 0x2deb33a0U,
                                  0x77037d81U, 0x63a440f2U, 0xf8bce6e5U,
                                  0xe12c4247U, 0x6b17d1f2U}),
              field_.encode(Limbs{0x37bf51f5U, 0xcbb64068U, 0x6b315eceU,
                                  0x2bce3357U, 0x7c0f9e16U, 0x8ee7eb4aU,
                                  0xfe1a7f9bU, 0x4fe342e2U}),
              field_.one()} {}

  auto base() const noexcept -> const Point& { return base_; }

  auto decode(Bytes encoded) const noexcept -> std::optional<Point> {
    if (encoded.size() != point_size ||
        encoded[0] != platform::sec1_uncompressed_tag)
      return std::nullopt;
    const Limbs plain_x = Field::big_endian(encoded.data() + 1);
    const Limbs plain_y = Field::big_endian(encoded.data() + 1 + scalar_size);
    if (Field::compare(plain_x, field_.modulus()) >= 0 ||
        Field::compare(plain_y, field_.modulus()) >= 0)
      return std::nullopt;
    const Limbs x = field_.encode(plain_x);
    const Limbs y = field_.encode(plain_y);
    const Limbs triple = field_.add(field_.add(x, x), x);
    const Limbs right = field_.add(
        field_.subtract(field_.multiply(field_.square(x), x), triple), curve_);
    if (field_.square(y) != right) return std::nullopt;
    return Point{x, y, field_.one()};
  }

  auto encode(const Point& point, std::uint8_t* output) const noexcept -> bool {
    if (Field::is_zero(point.z)) return false;
    const Limbs inverse = field_.invert(point.z);
    const Limbs square = field_.square(inverse);
    output[0] = platform::sec1_uncompressed_tag;
    Field::store_big_endian(field_.decode(field_.multiply(point.x, square)),
                            output + 1);
    Field::store_big_endian(field_.decode(field_.multiply(
                                point.y, field_.multiply(square, inverse))),
                            output + 1 + scalar_size);
    return true;
  }

  auto add(const Point& left, const Point& right) const noexcept -> Point {
    if (Field::is_zero(left.z)) return right;
    if (Field::is_zero(right.z)) return left;
    const Limbs left_square = field_.square(left.z);
    const Limbs right_square = field_.square(right.z);
    const Limbs u1 = field_.multiply(left.x, right_square);
    const Limbs u2 = field_.multiply(right.x, left_square);
    const Limbs s1 =
        field_.multiply(left.y, field_.multiply(right.z, right_square));
    const Limbs s2 =
        field_.multiply(right.y, field_.multiply(left.z, left_square));
    if (u1 == u2) return s1 == s2 ? twice(left) : Point{};
    const Limbs h = field_.subtract(u2, u1);
    const Limbs r = field_.subtract(s2, s1);
    const Limbs hh = field_.square(h);
    const Limbs hhh = field_.multiply(h, hh);
    const Limbs v = field_.multiply(u1, hh);
    const Limbs x = field_.subtract(field_.subtract(field_.square(r), hhh),
                                    field_.add(v, v));
    const Limbs y = field_.subtract(field_.multiply(r, field_.subtract(v, x)),
                                    field_.multiply(s1, hhh));
    return Point{x, y, field_.multiply(field_.multiply(left.z, right.z), h)};
  }

  auto multiply(const Limbs& scalar, const Point& point) const noexcept
      -> Point {
    Point result;
    for (int index = 255; index >= 0; --index) {
      result = twice(result);
      if (Field::bit(scalar, index)) result = add(result, point);
    }
    return result;
  }

 private:
  auto twice(const Point& point) const noexcept -> Point {
    if (Field::is_zero(point.z) || Field::is_zero(point.y)) return Point{};
    const Limbs delta = field_.square(point.z);
    const Limbs gamma = field_.square(point.y);
    const Limbs beta = field_.multiply(point.x, gamma);
    const Limbs product = field_.multiply(field_.subtract(point.x, delta),
                                          field_.add(point.x, delta));
    const Limbs alpha = field_.add(field_.add(product, product), product);
    const Limbs beta2 = field_.add(beta, beta);
    const Limbs beta4 = field_.add(beta2, beta2);
    const Limbs beta8 = field_.add(beta4, beta4);
    const Limbs x = field_.subtract(field_.square(alpha), beta8);
    const Limbs z = field_.subtract(
        field_.subtract(field_.square(field_.add(point.y, point.z)), gamma),
        delta);
    const Limbs gamma_square = field_.square(gamma);
    const Limbs gamma2 = field_.add(gamma_square, gamma_square);
    const Limbs gamma4 = field_.add(gamma2, gamma2);
    const Limbs gamma8 = field_.add(gamma4, gamma4);
    const Limbs y = field_.subtract(
        field_.multiply(alpha, field_.subtract(beta4, x)), gamma8);
    return Point{x, y, z};
  }

  Field field_{Limbs{0xffffffffU, 0xffffffffU, 0xffffffffU, 0x00000000U,
                     0x00000000U, 0x00000000U, 0x00000001U, 0xffffffffU}};
  Limbs curve_;
  Point base_;
};

SoftwareCrypto::SoftwareCrypto()
    : edwards_(std::make_unique<Edwards>()), p256_(std::make_unique<P256>()) {}

SoftwareCrypto::~SoftwareCrypto() = default;

auto SoftwareCrypto::view(Input input) noexcept -> Bytes {
  return Bytes{reinterpret_cast<const std::uint8_t*>(input.data()),
               input.size()};
}

auto SoftwareCrypto::raw(Output output) noexcept -> std::uint8_t* {
  return reinterpret_cast<std::uint8_t*>(output.data());
}

auto SoftwareCrypto::invalid() noexcept -> libchiaking::Unexpected<Error> {
  return libchiaking::unexpected(Error::invalid_argument);
}

auto SoftwareCrypto::sha256(Input input, Output digest) -> Outcome<void> {
  if (digest.size() != platform::sha256_digest_size) return invalid();
  Digest<Sha256>::compute(view(input), raw(digest));
  return {};
}

auto SoftwareCrypto::hmac_sha256(Input key, Input input, Output digest)
    -> Outcome<void> {
  if (digest.size() != platform::hmac_sha256_digest_size) return invalid();
  Hmac<Sha256>::compute(view(key), view(input), raw(digest));
  return {};
}

auto SoftwareCrypto::aes128_encrypt_block(Input key, Input block, Output output)
    -> Outcome<void> {
  if (key.size() != platform::aes128_key_size ||
      block.size() != platform::aes128_block_size ||
      output.size() != platform::aes128_block_size)
    return invalid();
  std::uint8_t encrypted[Aes128::block_size];
  Aes128{view(key).data()}.encrypt(view(block).data(), encrypted);
  std::copy(encrypted, encrypted + sizeof(encrypted), raw(output));
  return {};
}

auto SoftwareCrypto::aes128_gmac(Input key, Input iv, Input aad, Output tag)
    -> Outcome<void> {
  if (key.size() != platform::aes128_key_size ||
      iv.size() < platform::aes128_gcm_iv_size_min ||
      iv.size() > platform::aes128_gcm_iv_size_max ||
      tag.size() < platform::aes128_gcm_tag_size_min ||
      tag.size() > platform::aes128_gcm_tag_size_max)
    return invalid();
  std::uint8_t full[Gmac::tag_size];
  Gmac::compute(Aes128{view(key).data()}, view(iv), view(aad), full);
  std::copy(full, full + tag.size(), raw(tag));
  return {};
}

auto SoftwareCrypto::sha512(Input input, Output digest) -> Outcome<void> {
  if (digest.size() != platform::sha512_digest_size) return invalid();
  Digest<Sha512>::compute(view(input), raw(digest));
  return {};
}

auto SoftwareCrypto::hmac_sha512(Input key, Input input, Output digest)
    -> Outcome<void> {
  if (digest.size() != platform::hmac_sha512_digest_size) return invalid();
  Hmac<Sha512>::compute(view(key), view(input), raw(digest));
  return {};
}

auto SoftwareCrypto::hmac_sha1(Input key, Input input, Output digest)
    -> Outcome<void> {
  if (digest.size() != platform::hmac_sha1_digest_size) return invalid();
  Hmac<Sha1>::compute(view(key), view(input), raw(digest));
  return {};
}

auto SoftwareCrypto::scrypt(Input password, Input salt,
                            platform::ScryptParameters parameters,
                            Output output) -> Outcome<void> {
  if (output.empty() || parameters.cost < 2 ||
      (parameters.cost & (parameters.cost - 1)) != 0 ||
      parameters.block_size == 0 || parameters.parallelization == 0)
    return invalid();
  if (!Scrypt::derive(view(password), view(salt), parameters,
                      std::span<std::uint8_t>{raw(output), output.size()}))
    return libchiaking::unexpected(Error::backend_failure);
  return {};
}

auto SoftwareCrypto::ed25519_public_key(Input seed, Output public_key)
    -> Outcome<void> {
  if (seed.size() != platform::ed25519_seed_size ||
      public_key.size() != platform::ed25519_public_key_size)
    return invalid();
  edwards_->public_key(view(seed).data(), raw(public_key));
  return {};
}

auto SoftwareCrypto::ed25519_sign(Input seed, Input message, Output signature)
    -> Outcome<void> {
  if (seed.size() != platform::ed25519_seed_size ||
      signature.size() != platform::ed25519_signature_size)
    return invalid();
  std::uint8_t signed_bytes[Edwards::signature_size];
  edwards_->sign(view(seed).data(), view(message), signed_bytes);
  std::copy(signed_bytes, signed_bytes + sizeof(signed_bytes), raw(signature));
  return {};
}

auto SoftwareCrypto::ed25519_verify(Input public_key, Input message,
                                    Input signature) -> Outcome<bool> {
  if (public_key.size() != platform::ed25519_public_key_size ||
      signature.size() != platform::ed25519_signature_size)
    return invalid();
  return edwards_->verify(view(public_key).data(), view(message),
                          view(signature).data());
}

auto SoftwareCrypto::p256_base_multiply(Input scalar, Output point)
    -> Outcome<void> {
  if (scalar.size() != platform::p256_scalar_size ||
      point.size() != platform::p256_point_size)
    return invalid();
  std::uint8_t encoded[P256::point_size];
  if (!p256_->encode(p256_->multiply(Field::big_endian(view(scalar).data()),
                                     p256_->base()),
                     encoded))
    return invalid();
  std::copy(encoded, encoded + sizeof(encoded), raw(point));
  return {};
}

auto SoftwareCrypto::p256_multiply(Input scalar, Input point, Output product)
    -> Outcome<void> {
  if (scalar.size() != platform::p256_scalar_size ||
      product.size() != platform::p256_point_size)
    return invalid();
  const auto base = p256_->decode(view(point));
  std::uint8_t encoded[P256::point_size];
  if (!base ||
      !p256_->encode(
          p256_->multiply(Field::big_endian(view(scalar).data()), *base),
          encoded))
    return invalid();
  std::copy(encoded, encoded + sizeof(encoded), raw(product));
  return {};
}

auto SoftwareCrypto::p256_add(Input left, Input right, Output sum)
    -> Outcome<void> {
  if (sum.size() != platform::p256_point_size) return invalid();
  const auto first = p256_->decode(view(left));
  const auto second = p256_->decode(view(right));
  std::uint8_t encoded[P256::point_size];
  if (!first || !second || !p256_->encode(p256_->add(*first, *second), encoded))
    return invalid();
  std::copy(encoded, encoded + sizeof(encoded), raw(sum));
  return {};
}

}  // namespace test_support
