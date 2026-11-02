#include "tests/unit/core/net/kakusan/datagram_security_rig.hpp"

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/datagram_security.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace net_kakusan_test {
namespace {

namespace kakusan = libchiaking::net::kakusan;

class SecureDatagram : public DatagramSecurityRig {
 protected:
  static constexpr std::size_t gcm_record_overhead = 13 + 8 + 16;

  void expect_wrong_pin_fails(bool client_side) {
    auto pair = make_pair();
    const auto now = real_now();
    if (client_side)
      start(pair, now, flipped(pair.server->certificate_digest()),
            std::nullopt);
    else
      start(pair, now, std::nullopt,
            flipped(pair.client->certificate_digest()));
    const auto outcome = exchange(pair, now);
    EXPECT_EQ(outcome.client, Error::protocol);
    EXPECT_EQ(outcome.server, Error::protocol);
    expect_failed(*pair.client);
    expect_failed(*pair.server);
    EXPECT_TRUE(fails_with(pair.client->poll(now), Error::protocol));
    EXPECT_TRUE(
        fails_with(pair.server->receive(pattern(40, 1), now), Error::protocol));
  }
};

TEST_F(SecureDatagram, CompletesTheHandshakeWithinTheDatagramLimit) {
  auto pair = make_pair();
  EXPECT_EQ(pair.client->state(), State::idle);
  EXPECT_EQ(pair.server->state(), State::idle);
  EXPECT_NE(pair.client->certificate_digest(),
            pair.server->certificate_digest());
  EXPECT_EQ(pair.client->max_record_size(), 0U);
  EXPECT_FALSE(pair.client->next_deadline_us());
  EXPECT_TRUE(fails_with(pair.client->send(pattern(8, 1)), Error::protocol));

  const auto now = real_now();
  start(pair, now);
  EXPECT_EQ(pair.client->state(), State::handshaking);
  EXPECT_EQ(pair.server->state(), State::handshaking);
  EXPECT_TRUE(
      fails_with(pair.client->start(pair.server->certificate_digest(), now),
                 Error::protocol));

  const auto client_deadline = pair.client->next_deadline_us();
  ASSERT_TRUE(client_deadline);
  EXPECT_GT(*client_deadline, now + 300 * ms);
  EXPECT_LE(*client_deadline, now + kakusan::security_retransmit_initial_us);
  const auto server_deadline = pair.server->next_deadline_us();
  ASSERT_TRUE(server_deadline);
  EXPECT_EQ(*server_deadline, now + kakusan::security_handshake_timeout_us);

  std::size_t count = 0;
  std::size_t largest = 0;
  const auto outcome = exchange(pair, now, [&](Batch batch) {
    for (const auto& datagram : batch) {
      ++count;
      largest = std::max(largest, datagram.size());
    }
    return batch;
  });
  EXPECT_FALSE(outcome.client);
  EXPECT_FALSE(outcome.server);
  EXPECT_TRUE(pair.client->established());
  EXPECT_TRUE(pair.server->established());
  EXPECT_EQ(pair.client->state(), State::established);
  EXPECT_LE(largest, kakusan::max_datagram_size);
  EXPECT_EQ(pair.client->max_record_size(), pair.server->max_record_size());
  EXPECT_EQ(pair.client->max_record_size(),
            kakusan::max_datagram_size - gcm_record_overhead);
  EXPECT_FALSE(pair.client->take_record());
  EXPECT_FALSE(pair.server->take_record());
  EXPECT_FALSE(pair.client->next_deadline_us());
  RecordProperty("handshake_datagrams", std::to_string(count));
  RecordProperty("largest_datagram", std::to_string(largest));
  RecordProperty("max_record", std::to_string(pair.client->max_record_size()));
}

TEST_F(SecureDatagram, RefusesAnInvalidConfiguration) {
  Config unnamed;
  unnamed.alpn.clear();
  EXPECT_FALSE(
      kakusan::DatagramSecurity::open(provider(), Role::client, unnamed));
  Config oversized;
  oversized.alpn = std::string(256, 'a');
  EXPECT_FALSE(
      kakusan::DatagramSecurity::open(provider(), Role::server, oversized));
  Config config;
  config.record_limit = 0;
  EXPECT_FALSE(
      kakusan::DatagramSecurity::open(provider(), Role::client, config));
}

TEST_F(SecureDatagram, GivesEachConnectionItsOwnCertificate) {
  auto other = kakusan::DatagramSecurity::open(provider(), Role::client);
  ASSERT_TRUE(other);
  auto again = kakusan::DatagramSecurity::open(provider(), Role::client);
  ASSERT_TRUE(again);
  EXPECT_NE(other.value()->certificate_digest(),
            again.value()->certificate_digest());
}

TEST_F(SecureDatagram, CarriesRecordsWhole) {
  auto pair = established_pair();
  const auto max = pair.client->max_record_size();
  for (const std::size_t size :
       {std::size_t{1}, std::size_t{44}, std::size_t{976}, max}) {
    transfer(*pair.client, *pair.server, pattern(size, 3));
    transfer(*pair.server, *pair.client, pattern(size, 5));
  }

  EXPECT_TRUE(pair.client->send(pattern(max, 7)));
  const auto full = drain(*pair.client);
  ASSERT_EQ(full.size(), 1U);
  EXPECT_EQ(full[0].size(), kakusan::max_datagram_size);
  deliver(*pair.server, full, real_now());
  expect_record(*pair.server, pattern(max, 7));

  EXPECT_TRUE(
      fails_with(pair.client->send(pattern(max + 1, 9)), Error::protocol));
  EXPECT_TRUE(fails_with(pair.client->send({}), Error::protocol));
  EXPECT_TRUE(drain(*pair.client).empty());
  EXPECT_TRUE(pair.client->established());

  EXPECT_TRUE(pair.client->send(pattern(10, 11)));
  EXPECT_TRUE(pair.client->send(pattern(20, 13)));
  EXPECT_TRUE(pair.client->send(pattern(30, 15)));
  const auto three = drain(*pair.client);
  EXPECT_EQ(three.size(), 3U);
  deliver(*pair.server, three, real_now());
  expect_record(*pair.server, pattern(10, 11));
  expect_record(*pair.server, pattern(20, 13));
  expect_record(*pair.server, pattern(30, 15));
  EXPECT_FALSE(pair.server->take_record());
}

TEST_F(SecureDatagram, RefusesToExportBeforeEstablishment) {
  auto unestablished = make_pair();
  std::array<std::byte, 32> early{};
  EXPECT_TRUE(fails_with(
      unestablished.client->export_secret("EXPORTER-RCP-1-pairing", {}, early),
      Error::protocol));
  start(unestablished, real_now());
  EXPECT_TRUE(fails_with(
      unestablished.client->export_secret("EXPORTER-RCP-1-pairing", {}, early),
      Error::protocol));
}

TEST_F(SecureDatagram, ExportsMatchingSecretsSeparatedByLabelAndContext) {
  auto pair = established_pair();
  const auto context = pattern(32, 17);
  const auto other_context = pattern(32, 19);
  const std::array<std::byte, 32> zero{};
  std::array<std::byte, 32> client_secret{};
  std::array<std::byte, 32> server_secret{};
  EXPECT_TRUE(pair.client->export_secret("EXPORTER-RCP-1-pairing", context,
                                         client_secret));
  EXPECT_TRUE(pair.server->export_secret("EXPORTER-RCP-1-pairing", context,
                                         server_secret));
  EXPECT_EQ(client_secret, server_secret);
  EXPECT_NE(client_secret, zero);

  std::array<std::byte, 32> other_label{};
  EXPECT_TRUE(
      pair.client->export_secret("EXPORTER-RCP-1-media", context, other_label));
  EXPECT_NE(other_label, client_secret);

  std::array<std::byte, 32> other_secret{};
  EXPECT_TRUE(pair.client->export_secret("EXPORTER-RCP-1-pairing",
                                         other_context, other_secret));
  EXPECT_NE(other_secret, client_secret);

  std::array<std::byte, 32> empty_client{};
  std::array<std::byte, 32> empty_server{};
  EXPECT_TRUE(
      pair.client->export_secret("EXPORTER-RCP-1-pairing", {}, empty_client));
  EXPECT_TRUE(
      pair.server->export_secret("EXPORTER-RCP-1-pairing", {}, empty_server));
  EXPECT_EQ(empty_client, empty_server);
  EXPECT_NE(empty_client, client_secret);

  auto second = established_pair();
  std::array<std::byte, 32> second_secret{};
  EXPECT_TRUE(second.client->export_secret("EXPORTER-RCP-1-pairing", context,
                                           second_secret));
  EXPECT_NE(second_secret, client_secret);
}

TEST_F(SecureDatagram, FailsOnBothSidesWhenTheClientPinIsWrong) {
  expect_wrong_pin_fails(true);
}

TEST_F(SecureDatagram, FailsOnBothSidesWhenTheServerPinIsWrong) {
  expect_wrong_pin_fails(false);
}

TEST_F(SecureDatagram, FailsOnBothSidesWhenTheApplicationProtocolsDiffer) {
  Config server_config;
  server_config.alpn = "rcp/1/draft-06";
  auto pair = make_pair({}, std::move(server_config));
  const auto now = real_now();
  start(pair, now);
  const auto outcome = exchange(pair, now);
  EXPECT_EQ(outcome.client, Error::protocol);
  EXPECT_EQ(outcome.server, Error::protocol);
  expect_failed(*pair.client);
  expect_failed(*pair.server);
}

TEST_F(SecureDatagram, RetransmitsALostServerFlight) {
  auto pair = make_pair();
  const auto begin = real_now();
  start(pair, begin);
  deliver(*pair.server, drain(*pair.client), begin);
  const auto lost = drain(*pair.server);
  EXPECT_FALSE(lost.empty());
  const auto lost_at = real_now();
  const auto outcome = wait_for_retransmission(pair, lost_at);
  EXPECT_FALSE(outcome.client);
  EXPECT_FALSE(outcome.server);
  EXPECT_TRUE(pair.client->established());
  EXPECT_TRUE(pair.server->established());
}

TEST_F(SecureDatagram, AnswersALostFinalFlightAfterEstablishment) {
  auto pair = make_pair();
  const auto begin = real_now();
  start(pair, begin);
  deliver(*pair.server, drain(*pair.client), begin);
  deliver(*pair.client, drain(*pair.server), begin);
  deliver(*pair.server, drain(*pair.client), begin);
  EXPECT_TRUE(pair.server->established());
  EXPECT_FALSE(pair.client->established());
  const auto lost = drain(*pair.server);
  EXPECT_FALSE(lost.empty());
  const auto lost_at = real_now();
  const auto outcome = wait_for_retransmission(pair, lost_at);
  EXPECT_FALSE(outcome.client);
  EXPECT_FALSE(outcome.server);
  EXPECT_TRUE(pair.client->established());
  EXPECT_TRUE(pair.server->established());
  transfer(*pair.client, *pair.server, pattern(64, 21));
}

TEST_F(SecureDatagram, CompletesADuplicatedAndReorderedHandshake) {
  auto pair = make_pair();
  const auto now = real_now();
  start(pair, now);
  int batches = 0;
  const auto outcome = exchange(pair, now, [&](Batch batch) {
    if (batches++ >= 8) return batch;
    Batch mangled;
    for (auto datagram = batch.rbegin(); datagram != batch.rend(); ++datagram) {
      mangled.push_back(*datagram);
      mangled.push_back(*datagram);
    }
    return mangled;
  });
  EXPECT_FALSE(outcome.client);
  EXPECT_FALSE(outcome.server);
  EXPECT_TRUE(pair.client->established());
  EXPECT_TRUE(pair.server->established());
  transfer(*pair.server, *pair.client, pattern(100, 23));
}

TEST_F(SecureDatagram, DeliversAReplayedRecordOnce) {
  auto pair = established_pair();
  const auto now = real_now();
  EXPECT_TRUE(pair.client->send(pattern(50, 25)));
  const auto first = drain(*pair.client);
  EXPECT_TRUE(pair.client->send(pattern(60, 27)));
  const auto second = drain(*pair.client);
  EXPECT_EQ(first.size(), 1U);
  EXPECT_EQ(second.size(), 1U);

  deliver(*pair.server, second, now);
  deliver(*pair.server, second, now);
  deliver(*pair.server, first, now);
  deliver(*pair.server, first, now);
  deliver(*pair.server, second, now);
  expect_record(*pair.server, pattern(60, 27));
  expect_record(*pair.server, pattern(50, 25));
  EXPECT_FALSE(pair.server->take_record());
  EXPECT_TRUE(pair.server->established());
}

TEST_F(SecureDatagram, DropsCorruptedDatagrams) {
  auto pair = established_pair();
  const auto now = real_now();
  EXPECT_TRUE(pair.client->send(pattern(80, 29)));
  const auto batch = drain(*pair.client);
  ASSERT_EQ(batch.size(), 1U);
  const auto& original = batch[0];

  auto body = original;
  body[body.size() / 2] ^= std::byte{0x40};
  auto tag = original;
  tag.back() ^= std::byte{0x01};
  auto header = original;
  header[0] = std::byte{0x63};
  Datagram truncated(original.begin(), original.begin() + 9);
  Datagram oversized(kakusan::max_datagram_size + 1, std::byte{0x17});
  for (const auto& damaged :
       {body, tag, header, truncated, oversized, pattern(64, 31)}) {
    EXPECT_TRUE(pair.server->receive(damaged, now));
    EXPECT_FALSE(pair.server->take_record());
    EXPECT_TRUE(pair.server->established());
  }
  EXPECT_TRUE(pair.server->receive({}, now));

  deliver(*pair.server, batch, now);
  expect_record(*pair.server, pattern(80, 29));
  transfer(*pair.server, *pair.client, pattern(90, 33));
}

TEST_F(SecureDatagram, SurfacesNoRecordBeforeEstablishment) {
  auto pair = make_pair();
  const auto now = real_now();
  start(pair, now);
  deliver(*pair.server, drain(*pair.client), now);
  deliver(*pair.client, drain(*pair.server), now);
  deliver(*pair.server, drain(*pair.client), now);
  EXPECT_TRUE(pair.server->established());
  EXPECT_FALSE(pair.client->established());
  const auto final_flight = drain(*pair.server);
  EXPECT_FALSE(final_flight.empty());

  EXPECT_TRUE(pair.server->send(pattern(32, 35)));
  const auto early = drain(*pair.server);
  ASSERT_EQ(early.size(), 1U);
  EXPECT_TRUE(pair.client->receive(early[0], now));
  EXPECT_FALSE(pair.client->take_record());
  EXPECT_FALSE(pair.client->established());
  EXPECT_TRUE(fails_with(pair.client->send(pattern(8, 37)), Error::protocol));

  deliver(*pair.client, final_flight, now);
  EXPECT_TRUE(pair.client->established());
  const auto late = pair.client->take_record();
  RecordProperty("early_record_after_establishment",
                 late ? "delivered" : "dropped");
  EXPECT_TRUE(!late || std::ranges::equal(late->span(), pattern(32, 35)));
  transfer(*pair.client, *pair.server, pattern(16, 39));
}

TEST_F(SecureDatagram, FailsClosedAtTheSendRecordLimit) {
  Config limited;
  limited.record_limit = 3;
  auto pair = established_pair(limited, {});
  for (std::uint8_t index = 0; index < 3; ++index)
    transfer(*pair.client, *pair.server, pattern(16, index));
  EXPECT_TRUE(fails_with(pair.client->send(pattern(16, 4)), Error::protocol));
  expect_failed(*pair.client);
}

TEST_F(SecureDatagram, FailsClosedAtTheReceiveRecordLimit) {
  Config receive_limited;
  receive_limited.record_limit = 2;
  auto other = established_pair({}, receive_limited);
  transfer(*other.client, *other.server, pattern(16, 41));
  transfer(*other.client, *other.server, pattern(16, 43));
  EXPECT_TRUE(other.client->send(pattern(16, 45)));
  const auto batch = drain(*other.client);
  ASSERT_EQ(batch.size(), 1U);
  EXPECT_TRUE(
      fails_with(other.server->receive(batch[0], real_now()), Error::protocol));
  expect_failed(*other.server);
}

TEST_F(SecureDatagram, SendsCloseNotifyWhenClosed) {
  auto pair = established_pair();
  const auto now = real_now();
  EXPECT_TRUE(pair.server->send(pattern(24, 47)));
  deliver(*pair.client, drain(*pair.server), now);
  EXPECT_TRUE(pair.client->send(pattern(24, 49)));

  pair.client->close();
  EXPECT_EQ(pair.client->state(), State::closed);
  EXPECT_FALSE(pair.client->established());
  EXPECT_EQ(pair.client->max_record_size(), 0U);
  EXPECT_FALSE(pair.client->next_deadline_us());
  EXPECT_FALSE(pair.client->take_record());
  const auto notify = drain(*pair.client);
  EXPECT_EQ(notify.size(), 2U);
  EXPECT_TRUE(fails_with(pair.client->send(pattern(8, 51)), Error::protocol));
  std::array<std::byte, 32> secret{};
  EXPECT_TRUE(fails_with(
      pair.client->export_secret("EXPORTER-RCP-1-pairing", {}, secret),
      Error::protocol));
  EXPECT_TRUE(pair.client->receive(pattern(40, 53), now));
  EXPECT_TRUE(pair.client->poll(now));
  pair.client->close();
  EXPECT_EQ(pair.client->state(), State::closed);

  deliver(*pair.server, notify, now);
  EXPECT_EQ(pair.server->state(), State::closed);
  EXPECT_FALSE(pair.server->established());
  expect_record(*pair.server, pattern(24, 49));
  EXPECT_FALSE(pair.server->take_record());
  EXPECT_TRUE(fails_with(pair.server->send(pattern(8, 55)), Error::protocol));
  EXPECT_TRUE(drain(*pair.server).empty());
}

TEST_F(SecureDatagram, ClosesQuietlyDuringTheHandshake) {
  const auto now = real_now();
  auto handshaking = make_pair();
  start(handshaking, now);
  handshaking.client->close();
  EXPECT_EQ(handshaking.client->state(), State::closed);
  EXPECT_FALSE(handshaking.client->next_deadline_us());
  EXPECT_TRUE(handshaking.client->poll(now + 20 * sec));
}

TEST_F(SecureDatagram, RefusesToStartOnceClosed) {
  const auto now = real_now();
  auto other = make(Role::client);
  auto idle = make(Role::server);
  idle->close();
  EXPECT_EQ(idle->state(), State::closed);
  EXPECT_TRUE(fails_with(idle->start(other->certificate_digest(), now),
                         Error::protocol));
}

TEST_F(SecureDatagram, KeepsATakenRecordAfterTheConnectionIsReleased) {
  static_assert(!std::is_copy_constructible_v<libchiaking::SecretBytes>);
  static_assert(
      !std::is_copy_constructible_v<kakusan::transport_event::Record>);
  auto pair = established_pair();
  const auto expected = pattern(64, 61);
  EXPECT_TRUE(pair.client->send(expected));
  deliver(*pair.server, drain(*pair.client), real_now());
  auto record = pair.server->take_record();
  ASSERT_TRUE(record);
  pair.server->close();
  pair.server.reset();
  EXPECT_TRUE(std::ranges::equal(record->span(), expected));
  auto moved = std::move(*record);
  EXPECT_TRUE(std::ranges::equal(moved.span(), expected));
}

TEST_F(SecureDatagram, TimesTheHandshakeOnTheInjectedClock) {
  auto pair = make_pair();
  const std::uint64_t begin = 5 * sec;
  start(pair, begin);
  EXPECT_FALSE(drain(*pair.client).empty());
  const auto deadline = pair.client->next_deadline_us();
  ASSERT_TRUE(deadline);
  EXPECT_LE(*deadline, begin + kakusan::security_retransmit_initial_us);

  EXPECT_TRUE(pair.client->poll(begin + 14 * sec));
  EXPECT_FALSE(drain(*pair.client).empty());
  const auto later = pair.client->next_deadline_us();
  ASSERT_TRUE(later);
  EXPECT_GT(*later, begin + 14 * sec);
  EXPECT_LE(*later, begin + kakusan::security_handshake_timeout_us);

  EXPECT_TRUE(fails_with(
      pair.client->poll(begin + kakusan::security_handshake_timeout_us),
      Error::timeout));
  expect_failed(*pair.client, Error::timeout);
  EXPECT_TRUE(fails_with(pair.client->poll(begin + 16 * sec), Error::timeout));
  EXPECT_TRUE(fails_with(
      pair.server->receive(pattern(40, 57),
                           begin + kakusan::security_handshake_timeout_us),
      Error::timeout));
  expect_failed(*pair.server, Error::timeout);
}

}  // namespace
}  // namespace net_kakusan_test
