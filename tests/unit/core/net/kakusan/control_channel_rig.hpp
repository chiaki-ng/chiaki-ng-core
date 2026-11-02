#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/control_channel.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/support/kakusan/messages.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace net_kakusan_test {

class ControlChannelTest : public ::testing::Test {
 protected:
  using ControlChannel = libchiaking::net::kakusan::ControlChannel;
  using ControlFragment = libchiaking::net::kakusan::ControlFragment;
  using ControlKind = libchiaking::net::kakusan::ControlKind;
  using ControlTicket = libchiaking::net::kakusan::ControlTicket;
  using Error = libchiaking::net::kakusan::Error;
  using Message = libchiaking::net::kakusan::control::Message;
  using SourceSetup = libchiaking::net::kakusan::control::SourceSetup;
  using Codec = libchiaking::net::kakusan::TransportFrameCodec;
  using Records = std::vector<std::vector<std::byte>>;
  using Record = std::vector<std::byte>;
  using SentMessages = std::vector<std::pair<bool, Message>>;

  static constexpr std::uint64_t ms = 1000;
  static constexpr std::uint64_t sec = 1'000'000;
  static constexpr std::uint64_t start = 7 * sec;

  class Pair {
   public:
    explicit Pair(std::uint64_t now) : host(now), guest(now) {}

    auto settle(std::uint64_t now) -> std::size_t {
      std::size_t moved = 0;
      for (int round = 0; round < 64; ++round) {
        const auto from_host = polled(host, now);
        for (const auto& record : from_host) {
          if (auto message = received(guest, record, now))
            at_guest.push_back(std::move(*message));
        }
        const auto from_guest = polled(guest, now);
        for (const auto& record : from_guest) {
          if (auto message = received(host, record, now))
            at_host.push_back(std::move(*message));
        }
        moved += from_host.size() + from_guest.size();
        if (from_host.empty() && from_guest.empty()) break;
      }
      return moved;
    }

    ControlChannel host;
    ControlChannel guest;
    std::vector<Message> at_host;
    std::vector<Message> at_guest;
  };

  static auto polled(ControlChannel& channel, std::uint64_t now) -> Records {
    Records records;
    const auto result = channel.poll(now, records);
    EXPECT_TRUE(result);
    return records;
  }

  static auto poll_error(ControlChannel& channel, std::uint64_t now)
      -> std::optional<Error> {
    Records records;
    const auto result = channel.poll(now, records);
    EXPECT_FALSE(result);
    if (result) return std::nullopt;
    return result.error();
  }

  static auto received(ControlChannel& channel, const Record& record,
                       std::uint64_t now) -> std::optional<Message> {
    auto result = channel.receive(record, now);
    EXPECT_TRUE(result);
    if (!result) return std::nullopt;
    return std::move(result.value());
  }

  static auto receive_error(ControlChannel& channel,
                            std::span<const std::byte> record,
                            std::uint64_t now) -> std::optional<Error> {
    const auto result = channel.receive(record, now);
    EXPECT_FALSE(result);
    if (result) return std::nullopt;
    return result.error();
  }

  static auto enqueued(ControlChannel& channel, const Message& message,
                       std::uint64_t now) -> ControlTicket {
    const auto ticket = channel.enqueue(message, now);
    EXPECT_TRUE(ticket);
    if (!ticket) return {};
    return ticket.value();
  }

  static auto enqueued(ControlChannel& channel, const SourceSetup& setup,
                       std::uint64_t now) -> ControlTicket {
    return enqueued(channel, Message{setup.clone()}, now);
  }

  static auto ping_with(std::uint8_t fill)
      -> libchiaking::net::kakusan::control::Ping {
    libchiaking::net::kakusan::control::Ping ping;
    ping.nonce.fill(static_cast<std::byte>(fill));
    return ping;
  }

  static auto active_with(std::uint32_t generation)
      -> libchiaking::net::kakusan::control::Active {
    return libchiaking::net::kakusan::control::Active{
        libchiaking::net::kakusan::SeatBinding{1, generation}};
  }

  static auto encoded_text(const Message& message) -> std::string {
    const auto encoded =
        libchiaking::net::kakusan::control::MessageCodec::encode_secret(
            message);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::string{encoded.value().view()};
  }

  static auto encoded_text(const SourceSetup& setup) -> std::string {
    return encoded_text(Message{setup.clone()});
  }

  static auto fragments_of(std::uint32_t id, std::string_view text) -> Records {
    const auto fragments =
        Codec::encode_control_fragments(id, test_support::Bytes::of(text));
    EXPECT_TRUE(fragments);
    if (!fragments) return {};
    return fragments.value();
  }

  static auto fragments_of(std::uint32_t id, const Message& message)
      -> Records {
    return fragments_of(id, encoded_text(message));
  }

  static auto fragments_of(std::uint32_t id, const SourceSetup& setup)
      -> Records {
    return fragments_of(id, encoded_text(setup));
  }

  static auto padded_object(std::size_t size) -> std::string {
    EXPECT_GE(size, 8U);
    std::string text{"{\"a\":\""};
    text.append(size - 8, 'x');
    text.append("\"}");
    EXPECT_EQ(text.size(), size);
    return text;
  }

  static auto ack_record(std::uint32_t id) -> Record {
    const auto ack = Codec::encode_control_ack(id);
    return {ack.begin(), ack.end()};
  }

  static auto decoded(const Record& record) -> ControlFragment {
    const auto fragment = Codec::decode_control(record);
    EXPECT_TRUE(fragment);
    if (!fragment) return {};
    return fragment.value();
  }

  static auto raw_record(std::uint32_t id, std::uint16_t index,
                         std::uint16_t count, std::size_t size) -> Record {
    Record record;
    libchiaking::bytes::Appender writer{record};
    writer.text("RCC1");
    writer.u8(1);
    writer.u8(0);
    writer.u16_be(index);
    writer.u32_be(id);
    writer.u16_be(count);
    writer.u16_be(static_cast<std::uint16_t>(size));
    writer.bytes(std::vector<std::byte>(size, std::byte{'x'}));
    return record;
  }

  static auto total_bytes(const Records& records) -> std::size_t {
    std::size_t total = 0;
    for (const auto& record : records) total += record.size();
    return total;
  }

  static auto fixture_messages() -> SentMessages {
    using test_support::kakusan::Fixtures;
    const auto document = libchiaking::json::strict::Value::parse(
        Fixtures::text("control-flow.json"));
    EXPECT_TRUE(document);
    SentMessages messages;
    if (!document) return messages;
    for (const auto& step :
         *Fixtures::member(document.value(), "steps").array()) {
      auto message = libchiaking::net::kakusan::control::MessageCodec::decode(
          Fixtures::member(step, "message").canonical().value());
      EXPECT_TRUE(message);
      if (!message) continue;
      messages.emplace_back(
          Fixtures::string_of(Fixtures::member(step, "sender")) == "host",
          std::move(message.value()));
    }
    return messages;
  }

  static auto fixture_setup() -> SourceSetup {
    for (const auto& entry : fixture_messages()) {
      if (const auto* setup = std::get_if<SourceSetup>(&entry.second))
        return setup->clone();
    }
    ADD_FAILURE() << "control-flow.json has no source setup";
    return {};
  }

  static auto base64_length(std::size_t bytes) -> std::size_t {
    return (bytes * 4 + 2) / 3;
  }

  static auto setup_with_headers(const std::vector<std::size_t>& sizes)
      -> SourceSetup {
    auto setup = fixture_setup();
    const auto profile = setup.video_profiles[0];
    setup.video_profiles.clear();
    for (const auto size : sizes) {
      auto next = profile;
      next.header.assign(size, std::byte{7});
      setup.video_profiles.push_back(std::move(next));
    }
    return setup;
  }

  static auto setup_of_json_length(std::size_t target) -> SourceSetup {
    constexpr std::size_t profiles = 8;
    const auto probe =
        setup_with_headers(std::vector<std::size_t>(profiles, 1));
    const auto probe_length = encoded_text(probe).size();
    const auto base = probe_length - profiles * base64_length(1);
    EXPECT_GT(target, base);
    const auto wanted = target - base;
    for (std::size_t common = 1; common <= 512; ++common) {
      const auto fixed = (profiles - 1) * base64_length(common);
      if (fixed >= wanted) continue;
      for (std::size_t last = 1; last <= 512; ++last) {
        if (fixed + base64_length(last) != wanted) continue;
        std::vector<std::size_t> sizes(profiles - 1, common);
        sizes.push_back(last);
        auto setup = setup_with_headers(sizes);
        EXPECT_EQ(encoded_text(setup).size(), target);
        return setup;
      }
    }
    ADD_FAILURE() << "no setup encodes to " << target << " bytes";
    return {};
  }

  static auto json_length_for_total(std::size_t total) -> std::size_t {
    for (std::size_t length = 1;
         length <= libchiaking::net::kakusan::max_control_message_size;
         ++length) {
      if (length + libchiaking::net::kakusan::control_header_size *
                       Codec::control_fragment_count(length) ==
          total)
        return length;
    }
    ADD_FAILURE() << "no message length fills " << total << " bytes";
    return 0;
  }
};

}  // namespace net_kakusan_test
