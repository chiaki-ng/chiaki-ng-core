#include "tests/unit/api/network/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace network_test {
namespace {

using InMemoryTcpStream = InMemoryNetworkTest;

TEST_F(InMemoryTcpStream, IsOrderedAndReadInPartsThatFitTheBuffer) {
  auto pair = connected_pair(network);
  const auto listener_endpoint = pair.listener->local_endpoint().value();
  EXPECT_EQ(pair.client->peer_endpoint().value(), listener_endpoint);
  EXPECT_EQ(pair.server->local_endpoint().value(), listener_endpoint);
  EXPECT_EQ(pair.server->peer_endpoint().value(),
            pair.client->local_endpoint().value());
  EXPECT_NE(pair.client->local_endpoint().value().port, listener_endpoint.port);

  EXPECT_EQ(pair.client->send(bytes("abc")).value(), 3U);
  EXPECT_EQ(pair.client->send(bytes("def")).value(), 3U);
  EXPECT_EQ(pair.client->send(bytes("ghi")).value(), 3U);
  std::string collected;
  std::vector<std::size_t> sizes;
  std::array<std::byte, 4> small{};
  for (;;) {
    const auto received = pair.server->receive(small);
    if (!received) {
      EXPECT_EQ(received.error(), platform::NetError::would_block);
      break;
    }
    sizes.push_back(received.value());
    collected += text(std::span{small}.first(received.value()));
  }
  EXPECT_EQ(collected, "abcdefghi");
  EXPECT_EQ(sizes, (std::vector<std::size_t>{4, 4, 1}));

  EXPECT_EQ(pair.server->send(bytes("reply")).value(), 5U);
  std::array<std::byte, 64> large{};
  const auto reply = pair.client->receive(large);
  ASSERT_TRUE(reply);
  EXPECT_EQ(text(std::span{large}.first(reply.value())), "reply");
  EXPECT_EQ(pair.client->receive(large).error(),
            platform::NetError::would_block);
}

TEST_F(InMemoryTcpStream, EndsWhenThePeerCloses) {
  auto pair = connected_pair(network);
  EXPECT_TRUE(pair.client->send(bytes("tail")));
  EXPECT_TRUE(pair.client->close());
  EXPECT_EQ(pair.client->close().error(), platform::NetError::closed);
  std::array<std::byte, 16> buffer{};
  EXPECT_EQ(pair.client->receive(buffer).error(), platform::NetError::closed);
  EXPECT_EQ(wait_for(network, *pair.server, platform::PollEvent::readable),
            platform::PollEvent::readable);
  const auto data = pair.server->receive(buffer);
  ASSERT_TRUE(data);
  EXPECT_EQ(text(std::span{buffer}.first(data.value())), "tail");
  EXPECT_EQ(wait_for(network, *pair.server, platform::PollEvent::readable),
            platform::PollEvent::readable);
  EXPECT_EQ(pair.server->receive(buffer).error(), platform::NetError::closed);
  EXPECT_EQ(pair.server->receive(buffer).error(), platform::NetError::closed);
  EXPECT_EQ(pair.server->send(bytes("late")).error(),
            platform::NetError::closed);
}

TEST_F(InMemoryTcpStream, ReportsReadinessThroughThePoller) {
  auto server = listener(network);
  EXPECT_EQ(wait_for(network, *server, platform::PollEvent::readable, 20ms),
            platform::PollEvent::none);
  auto stream = client(network);
  EXPECT_EQ(stream->connect(server->local_endpoint().value()).error(),
            platform::NetError::would_block);
  EXPECT_EQ(wait_for(network, *server, platform::PollEvent::readable),
            platform::PollEvent::readable);
  EXPECT_EQ(wait_for(network, *stream, platform::PollEvent::writable),
            platform::PollEvent::writable);
  EXPECT_EQ(wait_for(network, *stream, platform::PollEvent::readable, 20ms),
            platform::PollEvent::none);
  auto accepted = server->accept();
  ASSERT_TRUE(accepted);
  EXPECT_EQ(wait_for(network, *server, platform::PollEvent::readable, 20ms),
            platform::PollEvent::none);
  auto peer = std::move(accepted.value());
  EXPECT_EQ(wait_for(network, *peer, platform::PollEvent::readable, 20ms),
            platform::PollEvent::none);
  EXPECT_TRUE(stream->send(bytes("z")));
  EXPECT_EQ(wait_for(network, *peer, platform::PollEvent::readable),
            platform::PollEvent::readable);
  const auto both =
      wait_for(network, *peer,
               platform::PollEvent::readable | platform::PollEvent::writable);
  EXPECT_EQ(both,
            (platform::PollEvent::readable | platform::PollEvent::writable));
}

TEST_F(InMemoryTcpStream, PollerBlocksUntilThePeerSends) {
  auto pair = connected_pair(network);
  auto poller = network.pollers().create_poller();
  ASSERT_TRUE(poller);
  const std::array<platform::PollRequest, 2> requests{
      {{pair.server.get(), 1, platform::PollEvent::readable},
       {pair.listener.get(), 2, platform::PollEvent::readable}}};
  auto sender =
      after_a_moment([&] { EXPECT_TRUE(pair.client->send(bytes("wake"))); });
  const auto result = poller.value()->wait(requests, 5s, platform::StopToken{});
  EXPECT_TRUE(sender.join());
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().ready.size(), 1U);
  EXPECT_EQ(result.value().ready.front().tag, 1U);

  auto waker = after_a_moment([&] { EXPECT_TRUE(poller.value()->wake()); });
  std::array<std::byte, 8> buffer{};
  EXPECT_TRUE(pair.server->receive(buffer));
  const auto woken = poller.value()->wait(requests, 5s, platform::StopToken{});
  EXPECT_TRUE(waker.join());
  ASSERT_TRUE(woken);
  EXPECT_TRUE(woken.value().woke);
  EXPECT_TRUE(woken.value().ready.empty());

  auto closer = after_a_moment([&] { EXPECT_TRUE(pair.client->close()); });
  const auto eof = poller.value()->wait(requests, 5s, platform::StopToken{});
  EXPECT_TRUE(closer.join());
  ASSERT_TRUE(eof);
  ASSERT_EQ(eof.value().ready.size(), 1U);
  EXPECT_EQ(eof.value().ready.front().tag, 1U);
}

TEST_F(InMemoryTcpStream, FailsSendsAndReceivesAsInjected) {
  auto pair = connected_pair(network);
  network.fail_sends_with(platform::NetError::connection_aborted);
  const auto failed = pair.client->send(bytes("x"));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), platform::NetError::connection_aborted);
  network.clear_failures();
  EXPECT_TRUE(pair.client->send(bytes("x")));
  network.fail_receives_with(platform::NetError::connection_reset);
  std::array<std::byte, 4> buffer{};
  const auto reset = pair.server->receive(buffer);
  ASSERT_FALSE(reset);
  EXPECT_EQ(reset.error(), platform::NetError::connection_reset);
  network.clear_failures();
  EXPECT_EQ(pair.server->receive(buffer).value(), 1U);
}

TEST_F(InMemoryTcpStream, SendsCanBeLimited) {
  auto pair = connected_pair(network);
  network.limit_stream_sends(2);
  EXPECT_EQ(pair.client->send(bytes("abcde")).value(), 2U);
  network.clear_failures();
  EXPECT_EQ(pair.client->send(bytes("cde")).value(), 3U);
  std::array<std::byte, 8> buffer{};
  EXPECT_EQ(pair.server->receive(buffer).value(), 5U);
  EXPECT_EQ(text(std::span{buffer}.first(5)), "abcde");
}

}  // namespace
}  // namespace network_test
