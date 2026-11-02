#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/dtls/fake/association.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>

namespace fake_secure_datagram_test {
namespace {

using namespace std::chrono_literals;
using libchiaking::platform::SecureDatagramError;
using libchiaking::platform::SecureDatagramRole;
using libchiaking::platform::SecureDatagramState;
using libchiaking::testing::FakeSecureDatagramProvider;
using Session = Association::Session;

TEST(FakeSecureDatagramHandshake, RefusesAnUnpinnedPeer) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  auto wrong = pair.server->certificate_digest();
  wrong[3] ^= std::byte{1};
  ASSERT_TRUE(pair.server->start(pair.client->certificate_digest(), 1s));
  ASSERT_TRUE(pair.client->start(wrong, 1s));
  ASSERT_TRUE(pair.settle(1s));
  EXPECT_EQ(pair.client_failure, SecureDatagramError::peer_not_pinned);
  EXPECT_EQ(pair.client->state(), SecureDatagramState::failed);
  EXPECT_EQ(pair.server_failure, SecureDatagramError::handshake_failed);
  EXPECT_EQ(pair.server->state(), SecureDatagramState::failed);
  const auto polled = pair.client->poll(2s);
  ASSERT_FALSE(polled);
  EXPECT_EQ(polled.error(), SecureDatagramError::peer_not_pinned);
  EXPECT_FALSE(pair.client->next_deadline());
}

TEST(FakeSecureDatagramHandshake, RefusesADifferentProtocol) {
  Association pair{"test/2"};
  ASSERT_TRUE(pair.opened());
  ASSERT_TRUE(pair.start(1s));
  ASSERT_TRUE(pair.settle(1s));
  EXPECT_EQ(pair.server_failure, SecureDatagramError::handshake_failed);
  EXPECT_EQ(pair.client_failure, SecureDatagramError::handshake_failed);
}

TEST(FakeSecureDatagramHandshake, RetransmitsALostFlightOnTheCallersClock) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  pair.provider.lose_datagrams(1);
  ASSERT_TRUE(pair.start(10s));
  EXPECT_TRUE(Association::drain(*pair.client).empty());
  EXPECT_EQ(pair.client->next_deadline(), Session::Instant{10s + 400ms});
  EXPECT_EQ(pair.server->next_deadline(), Session::Instant{25s});
  EXPECT_TRUE(pair.client->poll(10s + 399ms));
  EXPECT_TRUE(Association::drain(*pair.client).empty());
  EXPECT_TRUE(pair.client->poll(10s + 400ms));
  EXPECT_EQ(pair.client->next_deadline(), Session::Instant{10s + 1200ms});
  ASSERT_TRUE(pair.settle(10s + 400ms));
  EXPECT_EQ(pair.client->state(), SecureDatagramState::established);
  EXPECT_EQ(pair.server->state(), SecureDatagramState::established);
}

TEST(FakeSecureDatagramHandshake, TimesOutWhenThePeerStaysSilent) {
  Association pair;
  ASSERT_TRUE(pair.opened());
  pair.provider.lose_datagrams(FakeSecureDatagramProvider::every_datagram);
  ASSERT_TRUE(pair.start(1s));
  EXPECT_TRUE(pair.client->poll(15s));
  EXPECT_EQ(pair.client->state(), SecureDatagramState::handshaking);
  const auto expired = pair.client->poll(16s);
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error(), SecureDatagramError::handshake_timeout);
  EXPECT_EQ(pair.client->state(), SecureDatagramState::failed);
  const auto late = pair.server->receive(Association::bytes(40, 1), 16s);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), SecureDatagramError::handshake_timeout);
  const auto sent = pair.client->send(Association::bytes(4, 1));
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error(), SecureDatagramError::handshake_timeout);
}

TEST(FakeSecureDatagramProfile, AnInvalidProfileIsRefused) {
  FakeSecureDatagramProvider provider;
  auto unnamed = Association::profile(SecureDatagramRole::client, "", 4);
  const auto refused = provider.open(unnamed);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), SecureDatagramError::invalid_profile);
  EXPECT_FALSE(provider.open(
      Association::profile(SecureDatagramRole::client, "test/1", 0)));
  EXPECT_EQ(provider.opened(), 0U);
  EXPECT_TRUE(provider.open(
      Association::profile(SecureDatagramRole::server, "test/1", 1)));
  EXPECT_EQ(provider.opened(), 1U);
}

}  // namespace
}  // namespace fake_secure_datagram_test
