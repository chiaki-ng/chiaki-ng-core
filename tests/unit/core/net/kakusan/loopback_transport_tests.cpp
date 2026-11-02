#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace net_kakusan_test {
namespace {

namespace transport_event = libchiaking::net::kakusan::transport_event;
using libchiaking::net::kakusan::TransportEvent;
using libchiaking::net::kakusan::TransportRole;
using test_support::kakusan::Lane;
using test_support::kakusan::LoopbackExchange;
using test_support::kakusan::LoopbackFactory;
using test_support::kakusan::LoopbackLink;
using test_support::kakusan::LoopbackTransport;

class LoopbackPair : public ::testing::Test {
 protected:
  static auto events(LoopbackTransport& side, std::uint64_t now_us)
      -> std::vector<TransportEvent> {
    std::vector<TransportEvent> out;
    if (!side.pump(now_us, out)) ADD_FAILURE() << "pump failed";
    return out;
  }

  void establish() {
    ASSERT_TRUE(host.gather(0, std::nullopt));
    ASSERT_TRUE(guest.gather(0, std::nullopt));
    const auto host_events = events(host, 0);
    const auto guest_events = events(guest, 0);
    ASSERT_FALSE(host_events.empty());
    ASSERT_FALSE(guest_events.empty());
    const auto& host_local =
        std::get<transport_event::Gathered>(host_events.at(0)).local;
    const auto& guest_local =
        std::get<transport_event::Gathered>(guest_events.at(0)).local;
    ASSERT_TRUE(guest.connect(host_local, 0));
    EXPECT_TRUE(events(guest, 0).empty());
    ASSERT_TRUE(host.connect(guest_local, 0));
  }

  test_support::SoftwareCrypto crypto;
  std::shared_ptr<LoopbackLink> link{std::make_shared<LoopbackLink>(crypto)};
  LoopbackTransport host{link, TransportRole::host};
  LoopbackTransport guest{link, TransportRole::guest};
  std::array<std::byte, 4> record{std::byte{1}, std::byte{2}, std::byte{3},
                                  std::byte{4}};
};

TEST_F(LoopbackPair, RefusesToSendBeforeItConnects) {
  ASSERT_TRUE(host.gather(0, std::nullopt));
  EXPECT_FALSE(host.send_record(record));
}

TEST_F(LoopbackPair, EstablishesOnceBothSidesConnect) {
  ASSERT_NO_FATAL_FAILURE(establish());
  for (auto* side : {&host, &guest}) {
    const auto seen = events(*side, 0);
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<transport_event::Nominated>(seen[0]));
    EXPECT_TRUE(std::holds_alternative<transport_event::Established>(seen[1]));
  }
}

TEST_F(LoopbackPair, DeliversRecordsAndDatagramsToThePeer) {
  ASSERT_NO_FATAL_FAILURE(establish());
  events(host, 0);
  events(guest, 0);
  EXPECT_TRUE(host.send_record(record));
  EXPECT_TRUE(host.send_datagram(record));
  const auto delivered = events(guest, 0);
  ASSERT_EQ(delivered.size(), 2U);
  EXPECT_EQ(std::get<transport_event::Record>(delivered[0]).bytes.size(), 4U);
  EXPECT_EQ(std::get<transport_event::Datagram>(delivered[1]).bytes.size(), 4U);
  EXPECT_TRUE(events(host, 0).empty());
}

TEST_F(LoopbackPair, ExportsAgreeingSecretsSeparatedByLabelAndContext) {
  ASSERT_NO_FATAL_FAILURE(establish());
  const auto context = test_support::Bytes::of("transcript");
  std::array<std::byte, 32> host_pairing{};
  std::array<std::byte, 32> guest_pairing{};
  std::array<std::byte, 32> host_media{};
  EXPECT_TRUE(
      host.export_secret(libchiaking::net::kakusan::pairing_exporter_label,
                         context, host_pairing));
  EXPECT_TRUE(
      guest.export_secret(libchiaking::net::kakusan::pairing_exporter_label,
                          context, guest_pairing));
  EXPECT_TRUE(host.export_secret(
      libchiaking::net::kakusan::media_exporter_label, context, host_media));
  EXPECT_EQ(host_pairing, guest_pairing);
  EXPECT_NE(host_pairing, host_media);
  std::array<std::byte, 32> other_context{};
  EXPECT_TRUE(
      host.export_secret(libchiaking::net::kakusan::pairing_exporter_label,
                         test_support::Bytes::of("other"), other_context));
  EXPECT_NE(other_context, host_pairing);
}

class LoopbackPairEstablished : public LoopbackPair {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(establish());
    events(host, 0);
    events(guest, 0);
  }

  std::array<std::byte, 2> small{std::byte{7}, std::byte{8}};
};

TEST_F(LoopbackPairEstablished, HoldsPacketsForTheLinkDelay) {
  link->delay_us = 5000;
  EXPECT_TRUE(host.send_record(small));
  EXPECT_EQ(guest.next_deadline_us(), 5000U);
  EXPECT_TRUE(events(guest, 4999).empty());
  EXPECT_EQ(events(guest, 5000).size(), 1U);
}

TEST_F(LoopbackPairEstablished, DropsWhatTheLinkChoosesButCountsItAsSent) {
  link->drop = [](TransportRole role, Lane lane,
                  const std::vector<std::byte>&) {
    return role == TransportRole::guest && lane == Lane::datagram;
  };
  EXPECT_TRUE(guest.send_datagram(small));
  EXPECT_TRUE(guest.send_record(small));
  const auto seen = events(host, 20000);
  ASSERT_EQ(seen.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<transport_event::Record>(seen[0]));
  EXPECT_EQ(guest.datagrams_sent, 1U);
  EXPECT_EQ(guest.records_sent, 1U);
}

TEST_F(LoopbackPairEstablished, StopsSendingAndReceivingOnceClosed) {
  guest.close();
  EXPECT_FALSE(guest.send_record(small));
  EXPECT_TRUE(host.send_record(small));
  EXPECT_TRUE(events(guest, 40000).empty());
}

TEST_F(LoopbackPair, FailsWhenTheCertificateDigestDoesNotMatch) {
  ASSERT_TRUE(host.gather(0, std::nullopt));
  ASSERT_TRUE(guest.gather(0, std::nullopt));
  const auto host_events = events(host, 0);
  const auto guest_events = events(guest, 0);
  ASSERT_FALSE(host_events.empty());
  ASSERT_FALSE(guest_events.empty());
  auto host_local =
      std::get<transport_event::Gathered>(host_events.at(0)).local.clone();
  const auto guest_local =
      std::get<transport_event::Gathered>(guest_events.at(0)).local.clone();
  host_local.certificate_digest[0] ^= std::byte{1};
  EXPECT_TRUE(guest.connect(host_local, 0));
  EXPECT_TRUE(host.connect(guest_local, 0));
  const auto seen = events(host, 0);
  ASSERT_EQ(seen.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<transport_event::Failed>(seen[1]));
  std::array<std::byte, 1> one{std::byte{1}};
  EXPECT_FALSE(host.send_record(one));
}

TEST(LoopbackFactory, PairsEachRoleWithTheFirstLinkMissingIt) {
  test_support::SoftwareCrypto crypto;
  auto exchange = std::make_shared<LoopbackExchange>(crypto);
  LoopbackFactory host_factory{exchange};
  LoopbackFactory guest_factory{exchange};
  auto first_host = host_factory.create(TransportRole::host);
  auto second_host = host_factory.create(TransportRole::host);
  EXPECT_TRUE(first_host);
  EXPECT_TRUE(second_host);
  ASSERT_EQ(exchange->links.size(), 2U);
  auto first_guest = guest_factory.create(TransportRole::guest);
  EXPECT_TRUE(first_guest);
  ASSERT_EQ(exchange->links.size(), 2U);
  EXPECT_TRUE(exchange->links[0]->attached[0]);
  EXPECT_TRUE(exchange->links[0]->attached[1]);
  EXPECT_FALSE(exchange->links[1]->attached[1]);
}

TEST(LoopbackFactory, FailsOnceWhenAskedTo) {
  test_support::SoftwareCrypto crypto;
  auto exchange = std::make_shared<LoopbackExchange>(crypto);
  LoopbackFactory host_factory{exchange};
  LoopbackFactory guest_factory{exchange};
  ASSERT_TRUE(host_factory.create(TransportRole::host));
  ASSERT_TRUE(host_factory.create(TransportRole::host));
  ASSERT_TRUE(guest_factory.create(TransportRole::guest));
  guest_factory.fail_next = true;
  EXPECT_FALSE(guest_factory.create(TransportRole::guest));
  EXPECT_TRUE(guest_factory.create(TransportRole::guest));
  ASSERT_EQ(exchange->links.size(), 2U);
  EXPECT_TRUE(exchange->links[1]->attached[1]);
}

}  // namespace
}  // namespace net_kakusan_test
