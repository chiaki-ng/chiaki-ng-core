#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"
#include "tests/support/clock.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;
namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace datagram = libchiaking::net::transport::datagram;
namespace takion = libchiaking::net::protocol::takion;

class SimRandom final : public platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::PlatformError> override {
    for (auto& byte : output) byte = static_cast<std::byte>(++counter);
    return {};
  }

  std::uint8_t counter{};
};

class PeerTiming {
 public:
  static constexpr std::uint64_t injected_delay_us = 40'000;

  std::uint64_t echo_delay_us{injected_delay_us};
  std::optional<std::uint64_t> duplicate_after_us{};
  std::map<unsigned, std::uint64_t> late_echo_us{};
};

class Delivery {
 public:
  std::uint64_t at_us{};
  datagram::OwnedDatagram bytes;
};

class SimulatedGkPeer {
 public:
  explicit SimulatedGkPeer(test_support::FakeClock& clock,
                           PeerTiming timing = {})
      : clock_(clock), timing_(std::move(timing)) {}

  void on_client_datagram(const datagram::OwnedDatagram& datagram) {
    ASSERT_FALSE(datagram.empty());
    const auto family = static_cast<std::uint8_t>(datagram[0]) & 0x0fU;
    if (family ==
        static_cast<std::uint8_t>(takion::PacketFamily::audio_and_haptics)) {
      ++probes;
      if (datagram.size() != 0x224) {
        deliver(0, datagram);
        return;
      }
      const auto index = rtt_probes++;
      const auto late = timing_.late_echo_us.find(index);
      const auto delay = late == timing_.late_echo_us.end()
                             ? timing_.echo_delay_us
                             : late->second;
      deliver(delay, datagram);
      if (timing_.duplicate_after_us)
        deliver(delay + *timing_.duplicate_after_us, datagram);
      return;
    }
    if (family != static_cast<std::uint8_t>(takion::PacketFamily::control))
      return;
    const auto profile = takion::resolve_takion_wire_profile(9);
    ASSERT_TRUE(profile);
    auto plain = takion::PacketCodec::decode_plain_packet(datagram, *profile);
    ASSERT_TRUE(plain);
    auto chunk =
        takion::ControlChunkCodec::decode_packet(plain.value().payload);
    ASSERT_TRUE(chunk);
    if (const auto* init = std::get_if<takion::InitChunk>(&chunk.value())) {
      client_tag_ = init->values.tag;
      channel_.emplace(
          takion::ReliableChannel::create(
              channel_config(peer_tag,
                             init->values.initial_sequence_number.value))
              .value());
      deliver(0, control_packet(takion::InitAcknowledgementChunk{
                     takion::ControlHeader{client_tag_, {}, 0, 0},
                     takion::InitAcknowledgementValues{
                         {takion::VerificationTag{peer_tag},
                          takion::AdvertisedReceiveWindow{
                              takion::default_advertised_receive_window},
                          takion::StreamCount{100}, takion::StreamCount{100},
                          takion::InitialSequenceNumber{peer_tag}},
                         {}}}));
      return;
    }
    if (std::holds_alternative<takion::CookieChunk>(chunk.value())) {
      deliver(0, control_packet(takion::CookieAcknowledgementChunk{
                     takion::ControlHeader{client_tag_, {}, 0, 0}}));
      return;
    }
    const auto* data = std::get_if<takion::DataChunk>(&chunk.value());
    if (!data) return;
    ASSERT_GE(data->payload.size(), std::size_t{4});
    const auto sequence =
        (std::to_integer<std::uint32_t>(data->payload[0]) << 24U) |
        (std::to_integer<std::uint32_t>(data->payload[1]) << 16U) |
        (std::to_integer<std::uint32_t>(data->payload[2]) << 8U) |
        std::to_integer<std::uint32_t>(data->payload[3]);
    deliver(0, control_packet(takion::DataAcknowledgementChunk{
                   takion::ControlHeader{client_tag_, {}, 0, 0},
                   takion::DataAcknowledgement{sequence, 0x19000, {}, {}}}));
    const auto end = (data->header.flags & 0x01U) != 0;
    auto received = channel_->receive(data->payload, end, clock_.now_us);
    for (const auto& message : received.messages) {
      const takion::ControlCodec codec;
      auto decoded = codec.decode(message.payload);
      if (!decoded) continue;
      const auto* command = std::get_if<takion::Senkusha>(&decoded.value());
      if (!command) continue;
      if (const auto* mtu =
              std::get_if<takion::SenkushaMtuCommand>(&command->command)) {
        deliver(0, mtu_response(mtu->id, mtu->mtu_request));
      } else if (const auto* client_mtu =
                     std::get_if<takion::SenkushaClientMtuCommand>(
                         &command->command);
                 client_mtu && client_mtu->id == 1) {
        send_control(takion::Senkusha{takion::SenkushaClientMtuCommand{
            1, client_mtu->mtu_request, true, client_mtu->mtu_request}});
      }
    }
  }

  void close() { pending_.clear(); }

  auto next_delivery_us() const -> std::optional<std::uint64_t> {
    if (pending_.empty()) return std::nullopt;
    return pending_.front().at_us;
  }

  auto take_ready() -> std::optional<datagram::OwnedDatagram> {
    if (pending_.empty() || pending_.front().at_us > clock_.now_us)
      return std::nullopt;
    auto bytes = std::move(pending_.front().bytes);
    pending_.erase(pending_.begin());
    return bytes;
  }

  unsigned probes{};
  unsigned rtt_probes{};

 private:
  static constexpr std::uint32_t peer_tag = 0xa1b2c3d4;

  static auto encode_packet(takion::PacketFamily family,
                            std::span<const std::byte> payload,
                            std::uint32_t version) -> datagram::OwnedDatagram {
    const auto profile = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(profile);
    takion::PlainPacket value{
        takion::PacketType{static_cast<std::uint8_t>(family)},
        std::nullopt,
        {payload.begin(), payload.end()}};
    if (takion::PacketType::has_extended_header(
            static_cast<std::uint8_t>(family)) &&
        profile.value().features().contains(
            takion::Feature::extended_packet_headers))
      value.extended_header = takion::ExtendedPacketHeader{1, 2};
    auto plain =
        takion::PacketCodec::encode_plain_packet(value, profile.value());
    EXPECT_TRUE(plain);
    return std::vector<std::byte>(plain.value().begin(), plain.value().end());
  }

  template <class Chunk>
  static auto control_packet(Chunk chunk) -> datagram::OwnedDatagram {
    auto encoded = takion::ControlChunkCodec::encode_packet(
        takion::ControlChunkPacket{std::move(chunk)});
    EXPECT_TRUE(encoded);
    return encode_packet(takion::PacketFamily::control, encoded.value().bytes(),
                         9);
  }

  static auto channel_config(std::uint32_t local_sequence,
                             std::uint32_t remote_sequence)
      -> takion::ReliableChannelConfig {
    return {
        .initial_local_sequence = local_sequence,
        .initial_remote_sequence = remote_sequence,
        .maximum_message_size = 1024 * 1024,
        .maximum_fragment_size = 1200,
        .maximum_fragment_count = 1024,
        .retransmission_window_size = 256,
        .receive_reorder_size = 256,
        .retransmission_timeout_us = 100'000,
        .maximum_retries = 5,
    };
  }

  static auto mtu_response(std::uint32_t frame_index, std::uint32_t mtu)
      -> datagram::OwnedDatagram {
    takion::SenkushaProbeValues values;
    values.video = true;
    values.frame_index = static_cast<std::uint16_t>(frame_index);
    values.units_in_frame_total = 1;
    values.codec = 0xff;
    const auto profile = takion::resolve_takion_wire_profile(7);
    EXPECT_TRUE(profile);
    auto encoded = takion::PacketCodec::encode_senkusha_probe(
        values, profile.value(), takion::PlainEnvelope{});
    EXPECT_TRUE(encoded);
    auto datagram = encode_packet(takion::PacketFamily::video,
                                  encoded.value().encoded_av_payload, 7);
    datagram.resize(mtu - 28);
    return datagram;
  }

  void deliver(std::uint64_t delay_us, datagram::OwnedDatagram bytes) {
    Delivery delivery{clock_.now_us + delay_us, std::move(bytes)};
    const auto position =
        std::upper_bound(pending_.begin(), pending_.end(), delivery.at_us,
                         [](std::uint64_t at, const Delivery& value) {
                           return at < value.at_us;
                         });
    pending_.insert(position, std::move(delivery));
  }

  void send_control(const takion::ControlMessage& message) {
    const takion::ControlCodec codec;
    auto encoded = codec.encode(message);
    ASSERT_TRUE(encoded);
    auto sent =
        peer_.send(8, takion::MessageDataType{0}, encoded.value().bytes(), 0);
    ASSERT_TRUE(sent.has_value());
    for (const auto& fragment : sent->fragments)
      deliver(0, control_packet(takion::DataChunk{
                     takion::ControlHeader{client_tag_, {}, 0, 1},
                     std::vector<std::byte>(fragment.payload.begin(),
                                            fragment.payload.end())}));
  }

  test_support::FakeClock& clock_;
  PeerTiming timing_;
  takion::VerificationTag client_tag_{};
  std::optional<takion::ReliableChannel> channel_;
  takion::ReliableChannel peer_ =
      takion::ReliableChannel::create(channel_config(peer_tag, 0)).value();
  std::vector<Delivery> pending_;
};

class SimulatedTransport final : public datagram::DatagramTransport {
 public:
  explicit SimulatedTransport(SimulatedGkPeer& peer) : peer_(peer) {}
  ~SimulatedTransport() override { peer_.close(); }
  SimulatedTransport(const SimulatedTransport&) = delete;
  auto operator=(const SimulatedTransport&) -> SimulatedTransport& = delete;

  auto send(std::span<const std::byte> bytes)
      -> takion::Expected<std::size_t, datagram::TransportError> override {
    peer_.on_client_datagram({bytes.begin(), bytes.end()});
    return bytes.size();
  }

  auto receive() -> takion::Expected<std::optional<datagram::Datagram>,
                                     datagram::TransportError> override {
    auto ready = peer_.take_ready();
    if (!ready) return std::optional<datagram::Datagram>{};
    received_ = std::move(*ready);
    return std::optional<datagram::Datagram>{received_};
  }

  auto close() -> takion::Expected<void, datagram::TransportError> override {
    peer_.close();
    return {};
  }

 private:
  SimulatedGkPeer& peer_;
  datagram::OwnedDatagram received_;
};

class SimulatedOsWaiter final : public libchiaking::net::http::PollWaiter {
 public:
  static constexpr std::uint64_t coarse_sleep_us = 15'000;

  SimulatedOsWaiter(test_support::FakeClock& clock,
                    std::vector<SimulatedGkPeer*> peers)
      : clock_(clock), peers_(std::move(peers)) {}

  auto wait_milliseconds(int, const platform::StopToken&) -> bool override {
    clock_.now_us += coarse_sleep_us;
    return true;
  }

  auto wait_readable(std::span<const platform::Pollable* const>,
                     int timeout_milliseconds, const platform::StopToken&)
      -> bool override {
    const auto timeout_at =
        clock_.now_us +
        static_cast<std::uint64_t>(timeout_milliseconds) * 1000U;
    std::optional<std::uint64_t> ready;
    for (const auto* peer : peers_) {
      const auto next = peer->next_delivery_us();
      if (next && (!ready || *next < *ready)) ready = next;
    }
    if (ready && *ready <= clock_.now_us) return true;
    clock_.now_us = ready ? std::min(*ready, timeout_at) : timeout_at;
    return true;
  }

 private:
  test_support::FakeClock& clock_;
  std::vector<SimulatedGkPeer*> peers_;
};

class DatacenterCase {
 public:
  const char* name;
  PeerTiming timing;
  std::uint64_t expected_mean_us;
};

class SurveyRttAccuracy : public ::testing::Test {
 protected:
  static auto reference_cloud_rtt_ms(std::uint64_t mean_us) -> int {
    return static_cast<int>(mean_us / 1000);
  }

  auto job(SimulatedGkPeer& peer) -> std::unique_ptr<play::PingJob> {
    auto config =
        play::PingProfile{play::PingService::cloud_stream}.senkusha_config();
    config.profile.big.session_key =
        libchiaking::net::protocol::takion::SharedSecretText{
            "synthetic-cloud-key"};
    return play::PingJob::over(std::make_unique<SimulatedTransport>(peer),
                               clock, test_support::process_threading(), random,
                               std::move(config));
  }

  static auto measures_each_echo(
      const std::vector<gk::Ping>& rows,
      const std::vector<DatacenterCase>& cases,
      const std::vector<std::unique_ptr<SimulatedGkPeer>>& peers)
      -> testing::AssertionResult {
    for (std::size_t index = 0; index < cases.size(); ++index) {
      const auto& row = rows[index];
      if (row.name != cases[index].name)
        return testing::AssertionFailure()
               << "row " << index << " is " << row.name;
      if (!row.measured)
        return testing::AssertionFailure() << row.name << " was not measured";
      if (row.rtt_ms != reference_cloud_rtt_ms(cases[index].expected_mean_us))
        return testing::AssertionFailure()
               << row.name << " measured " << row.rtt_ms << " ms";
      if (row.rtts_ms != std::vector<int>{row.rtt_ms})
        return testing::AssertionFailure()
               << row.name << " reported other round trips";
      if (peers[index]->rtt_probes != 10)
        return testing::AssertionFailure()
               << row.name << " saw " << peers[index]->rtt_probes << " probes";
    }
    return testing::AssertionSuccess();
  }

  test_support::FakeClock clock{1'000'000};
  SimRandom random;
};

TEST_F(SurveyRttAccuracy, MeasuresTheEchoDelayOfOneDatacenter) {
  SimulatedGkPeer peer{clock};
  play::DatacenterSurvey::Jobs jobs;
  jobs.push_back(job(peer));
  auto owner = play::DatacenterSurvey::start(
      clock, {{"dc-a", "203.0.113.1", 2053, 20'000}}, std::move(jobs));
  ASSERT_TRUE(owner);
  SimulatedOsWaiter waiter{clock, {&peer}};
  const platform::StopToken stop;
  const auto result = owner.value().run(waiter, stop);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().timed_out);
  ASSERT_EQ(result.value().rows.size(), std::size_t{1});
  const auto& row = result.value().rows[0];
  EXPECT_TRUE(row.measured);
  EXPECT_GE(peer.probes, 10U);
  const auto measured_us = static_cast<std::int64_t>(row.rtt_ms) * 1000;
  EXPECT_LE(std::llabs(measured_us - static_cast<std::int64_t>(
                                         PeerTiming::injected_delay_us)),
            1'000);
  EXPECT_EQ(row.mtu_in, 1454);
  EXPECT_EQ(row.mtu_out, 1454);
}

TEST_F(SurveyRttAccuracy, MeasuresEachConcurrentDatacenterOnItsOwn) {
  const std::vector<DatacenterCase> cases{
      {"dc-25", {25'000, 10'000, {}}, 25'000},
      {"dc-40", {40'000, 5'000, {{3, 1'030'000}}}, 40'000},
      {"dc-12", {12'000, std::nullopt, {}}, 12'000},
      {"dc-101", {101'000, 50'000, {}}, 101'000},
  };
  std::vector<std::unique_ptr<SimulatedGkPeer>> peers;
  std::vector<SimulatedGkPeer*> peer_views;
  std::vector<gk::Datacenter> datacenters;
  play::DatacenterSurvey::Jobs jobs;
  for (const auto& value : cases) {
    peers.push_back(std::make_unique<SimulatedGkPeer>(clock, value.timing));
    peer_views.push_back(peers.back().get());
    jobs.push_back(job(*peers.back()));
    datacenters.push_back({value.name, "203.0.113.1", 2053, 20'000});
  }
  auto owner =
      play::DatacenterSurvey::start(clock, datacenters, std::move(jobs));
  ASSERT_TRUE(owner);
  SimulatedOsWaiter waiter{clock, peer_views};
  const platform::StopToken stop;
  const auto result = owner.value().run(waiter, stop);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().timed_out);
  const auto& rows = result.value().rows;
  ASSERT_EQ(rows.size(), cases.size());
  EXPECT_TRUE(measures_each_echo(rows, cases, peers));
  EXPECT_EQ(rows[0].rtt_ms, 25);
  EXPECT_EQ(rows[1].rtt_ms, 40);
  EXPECT_EQ(rows[2].rtt_ms, 12);
  EXPECT_EQ(rows[3].rtt_ms, 101);
  const auto selection = play::DatacenterRanking::rank(rows);
  ASSERT_TRUE(selection.fastest);
  EXPECT_EQ(selection.fastest->name, "dc-12");
  EXPECT_EQ(selection.fastest->rtt_ms, 12);
  ASSERT_EQ(selection.sorted.size(), std::size_t{4});
  EXPECT_EQ(selection.sorted[1].name, "dc-25");
  EXPECT_EQ(selection.sorted[2].name, "dc-40");
  EXPECT_EQ(selection.sorted[3].name, "dc-101");
  EXPECT_FALSE(play::DatacenterRanking::rank({rows[3]}).fastest);
}

}  // namespace
}  // namespace cloud_test
