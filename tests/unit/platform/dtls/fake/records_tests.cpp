#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/dtls/fake/association.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <type_traits>

namespace fake_secure_datagram_test {
namespace {

using namespace std::chrono_literals;
using libchiaking::SecretBytes;
using libchiaking::platform::SecureDatagramError;
using libchiaking::platform::SecureDatagramState;

static_assert(!std::is_copy_constructible_v<SecretBytes>);

TEST(FakeSecureDatagramRecords, CrossAPinnedAssociation) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  EXPECT_EQ(pair.client->state(), SecureDatagramState::idle);
  EXPECT_NE(pair.client->certificate_digest(),
            pair.server->certificate_digest());
  EXPECT_EQ(pair.client->max_record_size(), 0U);
  EXPECT_FALSE(pair.client->next_deadline());
  const auto early = pair.client->send(Association::bytes(8, 1));
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), SecureDatagramError::invalid_state);
  ASSERT_TRUE(pair.establish(5s));
  EXPECT_FALSE(pair.client->next_deadline());
  EXPECT_FALSE(pair.server->next_deadline());
  EXPECT_EQ(pair.client->max_record_size(), 1200U - 37U);
  const auto again = pair.client->start(pair.server->certificate_digest(), 5s);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), SecureDatagramError::invalid_state);

  const auto record = Association::bytes(pair.client->max_record_size(), 3);
  ASSERT_TRUE(pair.client->send(record));
  const auto sent = Association::drain(*pair.client);
  ASSERT_EQ(sent.size(), 1U);
  EXPECT_EQ(sent[0].size(), 1200U);
  EXPECT_EQ(std::search(sent[0].begin(), sent[0].end(), record.begin(),
                        record.begin() + 32),
            sent[0].end());
  EXPECT_FALSE(Association::deliver(*pair.server, sent, 5s));
  EXPECT_FALSE(Association::deliver(*pair.server, sent, 5s));
  const auto received = pair.server->take_record();
  ASSERT_TRUE(received);
  EXPECT_TRUE(std::ranges::equal(received->bytes(), record));
  EXPECT_FALSE(pair.server->take_record());
}

TEST(FakeSecureDatagramRecords, ATamperedRecordIsDroppedQuietly) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  ASSERT_TRUE(pair.establish(5s));
  ASSERT_TRUE(pair.client->send(Association::bytes(4, 9)));
  auto fresh = Association::drain(*pair.client);
  ASSERT_FALSE(fresh.empty());
  fresh[0].back() ^= std::byte{1};
  EXPECT_FALSE(Association::deliver(*pair.server, fresh, 5s));
  EXPECT_FALSE(pair.server->take_record());
}

TEST(FakeSecureDatagramRecords, OversizedAndEmptyRecordsAreRefused) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  ASSERT_TRUE(pair.establish(5s));
  const auto oversized = pair.client->send(
      Association::bytes(pair.client->max_record_size() + 1, 5));
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error(), SecureDatagramError::invalid_argument);
  const auto empty = pair.client->send({});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), SecureDatagramError::invalid_argument);
}

TEST(FakeSecureDatagramExporter, IsSharedAndDeterministic) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  const auto before = pair.client->export_secret("label", {}, 32);
  ASSERT_FALSE(before);
  EXPECT_EQ(before.error(), SecureDatagramError::invalid_state);
  ASSERT_TRUE(pair.establish(1s));
  const auto context = Association::bytes(16, 7);
  const auto client = pair.client->export_secret("label", context, 32);
  const auto server = pair.server->export_secret("label", context, 32);
  ASSERT_TRUE(client);
  ASSERT_TRUE(server);
  EXPECT_EQ(client.value().size(), 32U);
  EXPECT_TRUE(
      std::ranges::equal(client.value().bytes(), server.value().bytes()));
  const auto repeated = pair.client->export_secret("label", context, 32);
  ASSERT_TRUE(repeated);
  EXPECT_TRUE(
      std::ranges::equal(repeated.value().bytes(), client.value().bytes()));
  const auto other_label = pair.client->export_secret("other", context, 32);
  ASSERT_TRUE(other_label);
  EXPECT_FALSE(
      std::ranges::equal(other_label.value().bytes(), client.value().bytes()));
  const auto other_context = pair.client->export_secret("label", {}, 32);
  ASSERT_TRUE(other_context);
  EXPECT_FALSE(std::ranges::equal(other_context.value().bytes(),
                                  client.value().bytes()));
  const auto nothing = pair.client->export_secret("label", context, 0);
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error(), SecureDatagramError::invalid_argument);

  Association second;
  ASSERT_TRUE(second.opened());
  ASSERT_TRUE(second.establish(1s));
  const auto elsewhere = second.client->export_secret("label", context, 32);
  ASSERT_TRUE(elsewhere);
  EXPECT_FALSE(
      std::ranges::equal(elsewhere.value().bytes(), client.value().bytes()));
}

TEST(FakeSecureDatagramRecords, TheRecordLimitFailsClosed) {
  Association pair{"test/1", 2};
  ASSERT_TRUE(pair.opened());
  ASSERT_TRUE(pair.establish(1s));
  for (std::uint8_t index = 0; index < 2; ++index) {
    ASSERT_TRUE(pair.client->send(Association::bytes(8, index)));
    EXPECT_FALSE(Association::deliver(*pair.server,
                                      Association::drain(*pair.client), 1s));
    EXPECT_TRUE(pair.server->take_record());
  }
  const auto over = pair.client->send(Association::bytes(8, 9));
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error(), SecureDatagramError::record_limit);
  EXPECT_EQ(pair.client->state(), SecureDatagramState::failed);
}

TEST(FakeSecureDatagramRecords, CloseTellsThePeer) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  ASSERT_TRUE(pair.establish(1s));
  pair.client->close();
  EXPECT_EQ(pair.client->state(), SecureDatagramState::closed);
  EXPECT_EQ(pair.client->max_record_size(), 0U);
  EXPECT_FALSE(
      Association::deliver(*pair.server, Association::drain(*pair.client), 1s));
  EXPECT_EQ(pair.server->state(), SecureDatagramState::closed);
  const auto sent = pair.server->send(Association::bytes(4, 1));
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error(), SecureDatagramError::invalid_state);
  EXPECT_TRUE(pair.server->poll(2s));
}

}  // namespace
}  // namespace fake_secure_datagram_test
