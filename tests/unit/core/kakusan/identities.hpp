#pragma once

#include "libchiaking/kakusan_types.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "tests/support/setup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace kakusan_test {

class Identities {
 public:
  static auto from_seed(libchiaking::platform::CryptoProvider& crypto,
                        std::span<const std::byte> seed)
      -> libchiaking::kakusan::DeviceIdentity {
    libchiaking::kakusan::SavedIdentity saved;
    test_support::Setup::require(seed.size() == saved.seed.size(),
                                 "the seed has the identity size");
    std::ranges::copy(seed, saved.seed.begin());
    return test_support::Setup::value(
        libchiaking::kakusan::DeviceIdentity::restore(crypto, saved),
        "the identity restores");
  }

  static auto tagged(libchiaking::platform::CryptoProvider& crypto,
                     std::uint8_t tag) -> libchiaking::kakusan::DeviceIdentity {
    std::array<std::byte, libchiaking::kakusan::SavedIdentity::seed_size>
        seed{};
    for (std::size_t index = 0; index < seed.size(); ++index)
      seed[index] = static_cast<std::byte>(tag + index);
    return from_seed(crypto, seed);
  }

  static auto hint_of(const libchiaking::kakusan::IdentityKey& key)
      -> libchiaking::kakusan::IdentityHint {
    libchiaking::kakusan::IdentityHint::Bytes bytes{};
    std::ranges::copy(std::span{key.bytes()}.first(bytes.size()),
                      bytes.begin());
    return libchiaking::kakusan::IdentityHint{bytes};
  }
};

}  // namespace kakusan_test
