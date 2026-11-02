#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/kakusan/introductions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

class PairingVectors {
 public:
  enum class Side : std::uint8_t { host, guest };

  using Fixtures = test_support::kakusan::Fixtures;
  using Bytes = test_support::Bytes;

  PairingVectors(libchiaking::platform::CryptoProvider& crypto, Side side)
      : introductions(crypto) {
    context.transcript =
        Bytes::unhex_array<32>(Fixtures::string(document, "transcript"));
    context.invitation_id =
        Bytes::unhex_array<16>(Fixtures::string(document, "salt").substr(30));
    context = side == Side::host ? introductions.for_host(context)
                                 : introductions.for_guest(context);
    std::ranges::copy(
        Bytes::unhex_array<32>(Fixtures::string(document, "pairing_exporter")),
        exporter.begin());
    const auto host_proof =
        introductions.host_proof(context.transcript, exporter);
    const auto guest_proof =
        introductions.guest_proof(context.transcript, exporter);
    own_proof = side == Side::host ? host_proof : guest_proof;
    peer_proof = side == Side::host ? guest_proof : host_proof;
    std::ranges::copy(Bytes::unhex_array<32>(Fixtures::string(
                          document, side == Side::host ? "x" : "y")),
                      scalar.begin());
    pa = Bytes::unhex_array<65>(Fixtures::string(document, "pA"));
    pb = Bytes::unhex_array<65>(Fixtures::string(document, "pB"));
    ca = Bytes::unhex_array<64>(Fixtures::string(document, "cA"));
    cb = Bytes::unhex_array<64>(Fixtures::string(document, "cB"));
    host_code = libchiaking::net::kakusan::Code::parse(
                    Fixtures::string(document, "host_code"))
                    .value();
    guest_code = libchiaking::net::kakusan::Code::parse(
                     Fixtures::string(document, "guest_code"))
                     .value();
  }

  static auto wide_of(const libchiaking::net::kakusan::Scalar& value)
      -> std::vector<std::byte> {
    std::vector<std::byte> wide(libchiaking::net::kakusan::wide_scalar_size);
    std::ranges::copy(value, wide.begin() + 32);
    return wide;
  }

  static auto source_setup()
      -> libchiaking::net::kakusan::control::SourceSetup {
    auto decoded = flow_message("SOURCE_SETUP");
    if (!decoded) return {};
    return std::get<libchiaking::net::kakusan::control::SourceSetup>(
        std::move(*decoded));
  }

  static auto source_ready_hash() -> libchiaking::net::kakusan::Digest256 {
    const auto decoded = flow_message("SOURCE_READY");
    if (!decoded) return {};
    return std::get<libchiaking::net::kakusan::control::SourceReady>(*decoded)
        .setup_hash;
  }

  std::string document{Fixtures::text("vectors.json")};
  Introductions introductions;
  libchiaking::kakusan::PairingContext context;
  libchiaking::net::kakusan::Exporter exporter{};
  libchiaking::net::kakusan::control::Identity own_proof;
  libchiaking::net::kakusan::control::Identity peer_proof;
  libchiaking::net::kakusan::Scalar scalar{};
  libchiaking::net::kakusan::control::Share pa{};
  libchiaking::net::kakusan::control::Share pb{};
  libchiaking::net::kakusan::Digest512 ca{};
  libchiaking::net::kakusan::Digest512 cb{};
  libchiaking::net::kakusan::Code host_code{
      libchiaking::net::kakusan::Code::from_value(0).value()};
  libchiaking::net::kakusan::Code guest_code{
      libchiaking::net::kakusan::Code::from_value(0).value()};
  libchiaking::net::kakusan::control::SourceSetup setup{source_setup()};

 private:
  static auto flow_message(std::string_view type)
      -> std::optional<libchiaking::net::kakusan::control::Message> {
    const auto flow = libchiaking::json::strict::Value::parse(
        Fixtures::text("control-flow.json"));
    EXPECT_TRUE(flow);
    if (!flow) return std::nullopt;
    for (const auto& step : *Fixtures::member(flow.value(), "steps").array()) {
      const auto& item = Fixtures::member(step, "message");
      if (Fixtures::string_of(Fixtures::member(item, "type")) != type) continue;
      auto decoded = libchiaking::net::kakusan::control::MessageCodec::decode(
          item.canonical().value());
      EXPECT_TRUE(decoded);
      if (!decoded) return std::nullopt;
      return std::move(decoded).value();
    }
    ADD_FAILURE() << "control-flow.json has no " << type;
    return std::nullopt;
  }
};

}  // namespace kakusan_test
