#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/support/setup.hpp"

#include <span>
#include <string>

namespace kakusan_test {

class Introductions {
 public:
  using Identity = libchiaking::net::kakusan::control::Identity;
  using Context = libchiaking::kakusan::PairingContext;

  explicit Introductions(libchiaking::platform::CryptoProvider& crypto)
      : crypto_(&crypto) {
    const auto vectors = test_support::kakusan::Fixtures::text("vectors.json");
    host_seed_ = test_support::Bytes::unhex_array<32>(
        test_support::kakusan::Fixtures::string(vectors, "host_seed"));
    guest_seed_ = test_support::Bytes::unhex_array<32>(
        test_support::kakusan::Fixtures::string(vectors, "guest_seed"));
    host_key_ = test_support::Bytes::unhex_array<32>(
        test_support::kakusan::Fixtures::string(vectors, "host_key"));
    guest_key_ = test_support::Bytes::unhex_array<32>(
        test_support::kakusan::Fixtures::string(vectors, "guest_key"));
    hosting_.session_id = test_support::Bytes::unhex_array<16>(
        test_support::kakusan::Fixtures::string(vectors, "session_id"));
    hosting_.local_seats = 2;
  }

  auto host_key() const noexcept
      -> const libchiaking::net::kakusan::IdentityKey& {
    return host_key_;
  }
  auto guest_key() const noexcept
      -> const libchiaking::net::kakusan::IdentityKey& {
    return guest_key_;
  }
  auto hosting() const noexcept -> const Identity::Hosting& { return hosting_; }

  auto for_host(Context context) const -> Context {
    context.parties.host_identity = host_key_;
    context.parties.guest_identity = {};
    context.peer_hint =
        libchiaking::net::kakusan::ShortBlockCodec::hint_of(guest_key_);
    return context;
  }
  auto for_guest(Context context) const -> Context {
    context.parties.host_identity = {};
    context.parties.guest_identity = guest_key_;
    context.peer_hint =
        libchiaking::net::kakusan::ShortBlockCodec::hint_of(host_key_);
    return context;
  }

  auto host_proof(const libchiaking::net::kakusan::Transcript& transcript,
                  std::span<const std::byte> exporter) const -> Identity {
    auto proof = libchiaking::net::kakusan::IdentityProof::sign(
        *crypto_, libchiaking::net::kakusan::Role::host, transcript, exporter,
        host_seed_, hosting_);
    return test_support::Setup::value(std::move(proof), "the host proof signs");
  }
  auto guest_proof(const libchiaking::net::kakusan::Transcript& transcript,
                   std::span<const std::byte> exporter) const -> Identity {
    auto proof = libchiaking::net::kakusan::IdentityProof::sign(
        *crypto_, libchiaking::net::kakusan::Role::guest, transcript, exporter,
        guest_seed_, std::nullopt);
    return test_support::Setup::value(std::move(proof), "the guest proof signs");
  }

 private:
  libchiaking::platform::CryptoProvider* crypto_;
  libchiaking::net::kakusan::IdentitySeed host_seed_{};
  libchiaking::net::kakusan::IdentitySeed guest_seed_{};
  libchiaking::net::kakusan::IdentityKey host_key_{};
  libchiaking::net::kakusan::IdentityKey guest_key_{};
  Identity::Hosting hosting_{};
};

}  // namespace kakusan_test
