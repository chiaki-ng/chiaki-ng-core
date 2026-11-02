#include "tests/support/live_providers.hpp"
#include "tests/unit/core/net/protocol/takion/client_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

namespace takion_test {
namespace {

class TakionClientPump : public ClientRig {};

TEST_F(TakionClientPump, FlushingSendsPostedCommandsWithoutReceiving) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 1);
  EXPECT_TRUE(fixture.client->flush_commands());
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 0);
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  EXPECT_TRUE(fixture.client->flush_commands());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
}

TEST_F(TakionClientPump,
       RunsABoundedNumberOfCommandsInOrderAlongsideTheNetwork) {
  takion::ClientConfig config;
  config.limits.mailbox_capacity = 4;
  config.limits.maximum_commands_per_pump = 1;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());

  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->post(
      takion::SendControl{takion::Disconnect{"second", std::nullopt}}));
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));

  const takion::ControlCodec codec;
  auto inbound = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(inbound);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, inbound.value().bytes(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())}));

  const auto first_boundary = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.transport->incoming.empty());
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 2);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.events.count<takion::client_event::ControlReceived>(), 1);
  EXPECT_EQ(fixture.events.values<takion::client_event::ControlReceived>()
                .front()
                .message,
            takion::ControlMessage{takion::Heartbeat{}});
  auto first =
      decode_outbound_control(fixture.transport->sent, first_boundary, 1);
  auto first_decoded = codec.decode(first.payload);
  ASSERT_TRUE(first_decoded);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(first_decoded.value()));
  const auto second_boundary = fixture.transport->sent.size();

  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  auto second =
      decode_outbound_control(fixture.transport->sent, second_boundary, 1, 7,
                              takion::PlainEnvelope{}, 1, 0x11223345);
  auto second_decoded = codec.decode(second.payload);
  ASSERT_TRUE(second_decoded);
  const auto* disconnect =
      std::get_if<takion::Disconnect>(&second_decoded.value());
  ASSERT_TRUE(disconnect);
  EXPECT_EQ(disconnect->reason, "second");
  const auto third_boundary = fixture.transport->sent.size();

  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 0);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  auto third =
      decode_outbound_control(fixture.transport->sent, third_boundary, 1, 7,
                              takion::PlainEnvelope{}, 1, 0x11223346);
  auto third_decoded = codec.decode(third.payload);
  ASSERT_TRUE(third_decoded);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(third_decoded.value()));
}

TEST_F(TakionClientPump, KeepsTheClientUsableAfterASoftEncodeFailure) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_TRUE(fixture.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::video, {}, 9}));
  const auto soft_result = fixture.client->pump_once();
  ASSERT_FALSE(soft_result);
  EXPECT_EQ(soft_result.error(), takion::ClientError::packet_encode_failure);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  EXPECT_EQ(fixture.transport->close_count, 0);

  const auto sent_count = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{
      takion::CongestionValues{0x1122, 0x3344, 0x5566}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
  ASSERT_EQ(fixture.transport->sent.size(), sent_count + 1);
}

TEST_F(TakionClientPump, ReportsTheNextDeadlineOfQueuedCommandsAndResends) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_FALSE(fixture.client->next_deadline_us());
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_EQ(fixture.client->next_deadline_us(), fixture.clock.now_us);
  EXPECT_TRUE(fixture.client->pump_once());
  const auto sent = fixture.transport->sent.size();
  for (int pass = 0; pass < 8 && fixture.transport->sent.size() == sent;
       ++pass) {
    const auto due = fixture.client->next_deadline_us();
    ASSERT_TRUE(due);
    ASSERT_GT(*due, fixture.clock.now_us);
    EXPECT_LE(*due, 300'000);
    fixture.clock.now_us = *due - 1;
    EXPECT_TRUE(fixture.client->pump_once());
    ASSERT_EQ(fixture.transport->sent.size(), sent);
    fixture.clock.now_us = *due;
    EXPECT_TRUE(fixture.client->pump_once());
  }
  ASSERT_EQ(fixture.transport->sent.size(), sent + 1);
  EXPECT_EQ(fixture.clock.now_us, 300'000);

  fixture.transport->incoming.push_back(
      acknowledgement_of(data_sequence(fixture.transport->sent.back())));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_FALSE(fixture.client->next_deadline_us());
}

TEST_F(TakionClientPump, SkipsReadsAPollFoundEmpty) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  ClientPollable socket;
  ClientPollable other;
  fixture.transport->polled = &socket;
  fixture.transport->exact = true;

  auto calls = fixture.transport->receive_calls;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 1);

  calls = fixture.transport->receive_calls;
  fixture.client->note_readiness(socket, false);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls);

  calls = fixture.transport->receive_calls;
  for (int count = 0; count < 3; ++count)
    fixture.transport->incoming.push_back(
        datagram::OwnedDatagram{std::byte{0xff}});
  fixture.client->note_readiness(socket, true);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 4);
  EXPECT_TRUE(fixture.transport->incoming.empty());

  calls = fixture.transport->receive_calls;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls);

  calls = fixture.transport->receive_calls;
  fixture.client->note_readiness(other, false);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 1);

  calls = fixture.transport->receive_calls;
  fixture.transport->exact = false;
  fixture.client->note_readiness(socket, false);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 1);
  EXPECT_EQ(fixture.client->snapshot().state, takion::ClientState::connected);
}

TEST_F(TakionClientPump, KeepsReadingAReadableRouteUntilItDrains) {
  takion::ClientConfig config;
  config.limits.maximum_datagrams_per_pump = 2;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  ClientPollable socket;
  fixture.transport->polled = &socket;
  fixture.transport->exact = true;
  for (int count = 0; count < 3; ++count)
    fixture.transport->incoming.push_back(
        datagram::OwnedDatagram{std::byte{0xff}});

  auto calls = fixture.transport->receive_calls;
  fixture.client->note_readiness(socket, true);
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 2);
  ASSERT_EQ(fixture.transport->incoming.size(), 1);

  calls = fixture.transport->receive_calls;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls + 2);
  EXPECT_TRUE(fixture.transport->incoming.empty());

  calls = fixture.transport->receive_calls;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->receive_calls, calls);
}

TEST_F(TakionClientPump, KeepsABlockedSendForALaterPumpWithoutWaiting) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  fixture.clock.now_us = 10'000;
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->blocked_sends = 1;
  EXPECT_TRUE(fixture.client->post(
      takion::SendControl{takion::Heartbeat{}, takion::ControlOperationId{9}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.transport->writable_waits.empty());
  EXPECT_EQ(fixture.clock.now_us, 10'000);
  EXPECT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().unsent_datagrams, 1);
  EXPECT_EQ(fixture.client->next_deadline_us(),
            10'000 + takion::default_unsent_retry_interval_us);
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);

  fixture.clock.now_us = 10'000 + takion::default_unsent_retry_interval_us;
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.transport->writable_waits.empty());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 1);
  EXPECT_EQ(fixture.client->snapshot().unsent_datagrams, 0);
  const takion::ControlCodec codec;
  const auto sent =
      decode_outbound_control(fixture.transport->sent, sent_before, 1);
  const auto decoded = codec.decode(sent.payload);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(std::holds_alternative<takion::Heartbeat>(decoded.value()));
  EXPECT_EQ(fixture.events.count<takion::client_event::PacketRejected>(), 0);
}

TEST_F(TakionClientPump, ReportsABlockedProbeAtOnceAndNeverKeepsIt) {
  const std::vector<std::byte> payload{std::byte{0xaa}, std::byte{0xbb}};
  const auto expected_size =
      profile(7).packet_header_size(takion::PacketType{
          static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics)}) +
      payload.size();

  ClientHarness blocked;
  ASSERT_NO_FATAL_FAILURE(blocked.connect());
  blocked.clock.now_us = 10'000;
  const auto sent_before = blocked.transport->sent.size();
  blocked.transport->blocked_sends = 1;
  EXPECT_TRUE(blocked.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::audio, payload,
                        expected_size, takion::ProbeOperationId{8}, 15'000}));
  EXPECT_TRUE(blocked.client->pump_once());
  EXPECT_TRUE(blocked.transport->writable_waits.empty());
  EXPECT_EQ(blocked.clock.now_us, 10'000);
  EXPECT_EQ(blocked.transport->sent.size(), sent_before);
  EXPECT_EQ(blocked.client->snapshot().unsent_datagrams, 0);
  auto probes = blocked.events.values<takion::client_event::ProbeSent>();
  ASSERT_EQ(probes.size(), 1);
  EXPECT_FALSE(probes[0].delivered);
  EXPECT_TRUE(probes[0].blocked);
  EXPECT_EQ(probes[0].now_microseconds, 10'000);
  const auto rejected =
      blocked.events.values<takion::client_event::PacketRejected>();
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.front().error, takion::ClientError::send_blocked);
  EXPECT_TRUE(
      takion::is_transient_send_error(takion::ClientError::send_blocked));
  EXPECT_EQ(blocked.client->snapshot().state, takion::ClientState::connected);

  ClientHarness failed;
  ASSERT_NO_FATAL_FAILURE(failed.connect());
  failed.transport->fail_send = true;
  EXPECT_TRUE(failed.client->post(
      takion::SendProbe{takion::SenkushaProbeMedia::audio, payload,
                        expected_size, takion::ProbeOperationId{11}, 15'000}));
  EXPECT_TRUE(failed.client->pump_once());
  EXPECT_TRUE(failed.transport->writable_waits.empty());
  probes = failed.events.values<takion::client_event::ProbeSent>();
  ASSERT_EQ(probes.size(), 1);
  EXPECT_FALSE(probes[0].delivered);
  EXPECT_FALSE(probes[0].blocked);
  EXPECT_EQ(failed.events.values<takion::client_event::PacketRejected>()
                .front()
                .error,
            takion::ClientError::send_dropped);
}

TEST_F(TakionClientPump, QueuesEverySendBehindABlockedOneInOrder) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->blocked_sends = 1;
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{
      takion::CongestionValues{0x1122, 0x3344, 0x5566}}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->blocked_sends, 0);
  EXPECT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().unsent_datagrams, 2);

  fixture.clock.now_us += takion::default_unsent_retry_interval_us;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  EXPECT_EQ(fixture.client->snapshot().unsent_datagrams, 0);
  const auto first = takion::PacketCodec::decode_plain_packet(
      fixture.transport->sent[sent_before], profile(7));
  const auto second = takion::PacketCodec::decode_plain_packet(
      fixture.transport->sent[sent_before + 1], profile(7));
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first.value().type.family(), takion::PacketFamily::control);
  EXPECT_EQ(second.value().type.family(), takion::PacketFamily::congestion);
}

TEST_F(TakionClientPump, AFullUnsentQueueDropsOnlyBestEffortTrafficInOrder) {
  takion::ClientConfig config;
  config.limits.unsent_datagram_capacity = 2;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  const auto sent_before = fixture.transport->sent.size();
  fixture.transport->blocked_sends = 1'000;
  const takion::CongestionValues congestion{0x1122, 0x3344, 0x5566};
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{congestion}));
  EXPECT_TRUE(fixture.client->post(
      takion::SendControl{takion::Disconnect{"first", std::nullopt}}));
  EXPECT_TRUE(fixture.client->post(
      takion::SendControl{takion::Disconnect{"second", std::nullopt}}));
  EXPECT_TRUE(fixture.client->post(takion::SendCongestion{congestion}));
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_TRUE(fixture.transport->writable_waits.empty());
  EXPECT_EQ(fixture.transport->sent.size(), sent_before);
  EXPECT_EQ(fixture.client->snapshot().unsent_datagrams, 2);
  const auto rejected =
      fixture.events.values<takion::client_event::PacketRejected>();
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.front().error, takion::ClientError::send_blocked);

  fixture.transport->blocked_sends = 0;
  fixture.clock.now_us += takion::default_unsent_retry_interval_us;
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), sent_before + 2);
  const takion::ControlCodec codec;
  std::vector<std::string> reasons;
  for (std::size_t index = sent_before; index < sent_before + 2; ++index) {
    const auto control = decode_outbound_control(
        fixture.transport->sent, index, 1, 7, takion::PlainEnvelope{}, 1,
        data_sequence(fixture.transport->sent[index]));
    const auto decoded = codec.decode(control.payload);
    ASSERT_TRUE(decoded);
    const auto* disconnect = std::get_if<takion::Disconnect>(&decoded.value());
    ASSERT_TRUE(disconnect);
    reasons.push_back(disconnect->reason);
  }
  EXPECT_EQ(reasons, (std::vector<std::string>{"first", "second"}));
}

TEST_F(TakionClientPump, AcceptsPostsFromAnotherThreadWhileAPumpRuns) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  bool posted_during_pump{};
  bool all_accepted{};
  fixture.events.hook = [&](const takion::Event& event) {
    if (!std::holds_alternative<takion::client_event::ControlReceived>(event))
      return;
    std::atomic<bool> finished{};
    std::atomic<bool> accepted{};
    auto poster = test_support::LiveProviders::spawn(
        "takion poster", [&fixture, &finished, &accepted] {
          bool ok = true;
          for (const auto* reason : {"1", "2", "3"}) {
            ok = static_cast<bool>(fixture.client->post(takion::SendControl{
                     takion::Disconnect{reason, std::nullopt}})) &&
                 ok;
          }
          accepted.store(ok, std::memory_order_relaxed);
          finished.store(true, std::memory_order_release);
        });
    const auto deadline = test_support::LiveProviders::steady_us() + 5'000'000U;
    while (!finished.load(std::memory_order_acquire) &&
           test_support::LiveProviders::steady_us() < deadline) {
      test_support::LiveProviders::spin_pause();
    }
    posted_during_pump = finished.load(std::memory_order_acquire);
    EXPECT_TRUE(poster.join());
    all_accepted = posted_during_pump && accepted.load();
  };

  const takion::ControlCodec codec;
  auto inbound = codec.encode(takion::Heartbeat{});
  ASSERT_TRUE(inbound);
  takion::ReliableChannel peer =
      takion::ReliableChannel::create(peer_channel_config(1, 0xa1b2c3d4))
          .value();
  auto sent =
      peer.send(1, takion::MessageDataType{0}, inbound.value().bytes(), 0);
  ASSERT_EQ(sent->fragments.size(), 1);
  fixture.transport->incoming.push_back(control_packet(takion::DataChunk{
      takion::ControlHeader{takion::VerificationTag{0x11223344}, {}, 0, 1},
      std::vector<std::byte>(sent->fragments[0].payload.begin(),
                             sent->fragments[0].payload.end())}));
  EXPECT_TRUE(fixture.client->pump_once());
  fixture.events.hook = nullptr;
  EXPECT_TRUE(posted_during_pump);
  EXPECT_TRUE(all_accepted);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 3);
  EXPECT_EQ(fixture.client->next_deadline_us(), fixture.clock.now_us);

  const auto boundary = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), boundary + 3);
  std::vector<std::string> reasons;
  for (std::size_t index = boundary; index < boundary + 3; ++index) {
    const auto control = decode_outbound_control(
        fixture.transport->sent, index, 1, 7, takion::PlainEnvelope{}, 1,
        data_sequence(fixture.transport->sent[index]));
    const auto decoded = codec.decode(control.payload);
    ASSERT_TRUE(decoded);
    const auto* disconnect = std::get_if<takion::Disconnect>(&decoded.value());
    ASSERT_TRUE(disconnect);
    reasons.push_back(disconnect->reason);
  }
  EXPECT_EQ(reasons, (std::vector<std::string>{"1", "2", "3"}));
}

TEST_F(TakionClientPump, KeepsEachPostersOrderAcrossConcurrentPosters) {
  ClientHarness fixture;
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  constexpr std::size_t per_poster = 6;
  std::atomic<int> arrived{0};
  const auto post_all = [&](char poster) {
    arrived.fetch_add(1);
    while (arrived.load() < 2) {
    }
    for (std::size_t index = 0; index < per_poster; ++index) {
      const std::string reason{poster, static_cast<char>('0' + index)};
      EXPECT_TRUE(fixture.client->post(
          takion::SendControl{takion::Disconnect{reason, std::nullopt}}));
    }
  };
  auto first = test_support::LiveProviders::spawn("takion poster a",
                                                  [&] { post_all('a'); });
  auto second = test_support::LiveProviders::spawn("takion poster b",
                                                   [&] { post_all('b'); });
  EXPECT_TRUE(first.join());
  EXPECT_TRUE(second.join());
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 2 * per_poster);

  const auto boundary = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  ASSERT_EQ(fixture.transport->sent.size(), boundary + 2 * per_poster);
  const takion::ControlCodec codec;
  std::vector<std::string> from_a;
  std::vector<std::string> from_b;
  for (std::size_t index = boundary; index < fixture.transport->sent.size();
       ++index) {
    const auto control = decode_outbound_control(
        fixture.transport->sent, index, 1, 7, takion::PlainEnvelope{}, 1,
        data_sequence(fixture.transport->sent[index]));
    const auto decoded = codec.decode(control.payload);
    ASSERT_TRUE(decoded);
    const auto* disconnect = std::get_if<takion::Disconnect>(&decoded.value());
    ASSERT_TRUE(disconnect);
    if (disconnect->reason.front() == 'a')
      from_a.push_back(disconnect->reason);
    else
      from_b.push_back(disconnect->reason);
  }
  EXPECT_EQ(from_a,
            (std::vector<std::string>{"a0", "a1", "a2", "a3", "a4", "a5"}));
  EXPECT_EQ(from_b,
            (std::vector<std::string>{"b0", "b1", "b2", "b3", "b4", "b5"}));
}

TEST_F(TakionClientPump, RefusesAPostWhenTheCommandRingIsFull) {
  takion::ClientConfig config;
  config.limits.mailbox_capacity = 2;
  ClientHarness fixture{config};
  ASSERT_NO_FATAL_FAILURE(fixture.connect());
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
  const auto refused =
      fixture.client->post(takion::SendControl{takion::Heartbeat{}});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), takion::ClientError::mailbox_full);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 2);

  const auto boundary = fixture.transport->sent.size();
  EXPECT_TRUE(fixture.client->pump_once());
  EXPECT_EQ(fixture.transport->sent.size(), boundary + 2);
  EXPECT_EQ(fixture.client->snapshot().queued_commands, 0);
  EXPECT_TRUE(fixture.client->post(takion::SendControl{takion::Heartbeat{}}));
}

}  // namespace
}  // namespace takion_test
