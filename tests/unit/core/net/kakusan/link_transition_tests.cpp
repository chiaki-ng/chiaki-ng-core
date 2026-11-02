#include "src/core/net/kakusan/link.hpp"

#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace libchiaking::net::kakusan;

class ScriptedTransport final : public SpokeTransport {
 public:
  struct Tally {
    std::size_t records{};
    std::size_t datagrams{};
    std::size_t closes{};
    std::deque<std::vector<TransportEvent>> script;
    std::optional<Error> pump_error;
  };

  explicit ScriptedTransport(Tally& tally) : tally_(tally) {}

  auto gather(std::uint64_t, std::optional<IceCredentials>)
      -> Result<void> override {
    return {};
  }
  auto connect(const TransportDescription&, std::uint64_t)
      -> Result<void> override {
    return {};
  }
  auto pump(std::uint64_t, std::vector<TransportEvent>& events)
      -> Result<void> override {
    if (!tally_.script.empty()) {
      for (auto& event : tally_.script.front())
        events.push_back(std::move(event));
      tally_.script.pop_front();
    }
    if (tally_.pump_error) return fail(*std::exchange(tally_.pump_error, {}));
    return {};
  }
  auto next_deadline_us() const noexcept
      -> std::optional<std::uint64_t> override {
    return 5'000'000;
  }
  auto send_record(std::span<const std::byte>) -> Result<void> override {
    ++tally_.records;
    return {};
  }
  auto send_datagram(std::span<const std::byte>) -> Result<void> override {
    ++tally_.datagrams;
    return {};
  }
  auto export_secret(std::string_view, std::span<const std::byte>,
                     std::span<std::byte> output) -> Result<void> override {
    std::ranges::fill(output, std::byte{0x5a});
    return {};
  }
  void close() noexcept override { ++tally_.closes; }

 private:
  Tally& tally_;
};

class LinkTransitions : public ::testing::Test {
 protected:
  static constexpr std::uint64_t now = 1'000'000;

  struct Rig {
    explicit Rig(TransportRole role)
        : link(std::make_unique<ScriptedTransport>(tally), role) {}

    auto pump(std::vector<TransportEvent> events) -> std::span<LinkEvent> {
      tally.script.push_back(std::move(events));
      return link.pump(now);
    }

    ScriptedTransport::Tally tally;
    Link link;
  };

  static auto one(TransportEvent event) -> std::vector<TransportEvent> {
    std::vector<TransportEvent> items;
    items.push_back(std::move(event));
    return items;
  }

  static auto description() -> TransportDescription {
    auto credentials =
        IceCredentials::make(libchiaking::SecretString{"1111111111111111"},
                             libchiaking::SecretString{std::string(48, '3')});
    auto ice = IceParameters::make(
        std::move(credentials).value(),
        {Candidate{
            "1",
            2130706431,
            libchiaking::platform::SocketEndpoint{
                libchiaking::platform::parse_ip_address("192.0.2.10").value(),
                50000},
            CandidateType::host,
            {}}},
        0);
    return TransportDescription{std::move(ice).value(), {}};
  }

  static auto record(std::string_view prefix, std::size_t extra = 8)
      -> transport_event::Record {
    std::vector<std::byte> bytes;
    for (const char c : prefix) bytes.push_back(static_cast<std::byte>(c));
    bytes.resize(bytes.size() + extra, std::byte{0xee});
    return transport_event::Record{
        libchiaking::SecretBytes::from_bytes(bytes).value()};
  }

  static auto input_record() -> transport_event::Record {
    InputFrame frame;
    frame.binding = SeatBinding{1, 1};
    const auto bytes = ControllerFrameCodec::encode_input(frame).value();
    return transport_event::Record{
        libchiaking::SecretBytes::from_bytes(bytes).value()};
  }

  static auto feedback_record() -> transport_event::Record {
    FeedbackFrame frame;
    frame.binding = SeatBinding{1, 1};
    const auto bytes = ControllerFrameCodec::encode_feedback(frame).value();
    return transport_event::Record{
        libchiaking::SecretBytes::from_bytes(bytes).value()};
  }

  static auto events(std::span<const LinkEvent> items) -> std::string {
    std::string text;
    for (const auto& item : items) {
      text += std::format("{} ", item.index());
      if (const auto* malformed = std::get_if<link_event::Malformed>(&item))
        text += std::format("m{} ", static_cast<int>(malformed->error));
      if (const auto* failed = std::get_if<link_event::Failed>(&item))
        text += std::format("f{} ", static_cast<int>(failed->error));
      if (const auto* acked = std::get_if<link_event::Acked>(&item))
        text += std::format("a{} ", acked->token);
    }
    return text;
  }

  static auto outcome(const Result<void>& result) -> std::string {
    return result ? "ok "
                  : std::format("e{} ", static_cast<int>(result.error()));
  }

  static auto state(Rig& rig) -> std::string {
    return std::format("|{}{}{} r{} d{} c{}", rig.link.closed() ? "C" : "",
                       rig.link.established() ? "E" : "",
                       rig.link.next_deadline_us() ? "T" : "",
                       rig.tally.records, rig.tally.datagrams,
                       rig.tally.closes);
  }

  using Step = std::function<std::string(Rig&)>;

  static auto pumping(std::function<std::vector<TransportEvent>()> make)
      -> Step {
    return [make](Rig& rig) { return events(rig.pump(make())); };
  }

  struct Row {
    std::string_view name;
    TransportRole role;
    std::vector<Step> setup;
  };

  static auto established() -> Step {
    return [](Rig& rig) {
      static_cast<void>(rig.link.connect(description(), Transcript{}, now));
      return events(rig.pump(one(transport_event::Established{})));
    };
  }

  static auto rows() -> std::vector<Row> {
    const Step garbage = pumping([] {
      std::vector<TransportEvent> items;
      items.emplace_back(record("XXXX"));
      return items;
    });
    const Step failed =
        pumping([] { return one(transport_event::Failed{Error::timeout}); });
    const Step close = [](Rig& rig) {
      rig.link.close();
      return std::string{};
    };
    const Step queue = [](Rig& rig) {
      return outcome(rig.link.send_control(
          control::Message{control::AdmissionPending{}}, 7, false, now));
    };
    const Step terminal = [](Rig& rig) {
      return outcome(rig.link.send_control(
          control::Message{control::End{control::Reason::user}}, 9, true, now));
    };
    return {
        {"fresh", TransportRole::host, {}},
        {"connected", TransportRole::host, {[](Rig& rig) {
           return outcome(rig.link.connect(description(), Transcript{}, now));
         }}},
        {"established", TransportRole::host, {established()}},
        {"queued", TransportRole::host, {established(), queue}},
        {"terminal", TransportRole::host, {established(), terminal}},
        {"deaf", TransportRole::host, {established(), garbage}},
        {"stopped", TransportRole::host, {established(), failed}},
        {"closed", TransportRole::host, {established(), close}},
        {"stopped_closed", TransportRole::host, {established(), failed, close}},
        {"deaf_stopped", TransportRole::host, {established(), garbage, failed}},
        {"guest_established", TransportRole::guest, {established()}},
        {"guest_deaf", TransportRole::guest, {established(), garbage}},
        {"guest_stopped", TransportRole::guest, {established(), failed}},
    };
  }

  static auto columns() -> std::vector<std::pair<std::string_view, Step>> {
    return {
        {"gather",
         [](Rig& rig) { return outcome(rig.link.gather(now, std::nullopt)); }},
        {"connect",
         [](Rig& rig) {
           return outcome(rig.link.connect(description(), Transcript{}, now));
         }},
        {"pump_empty", pumping([] { return std::vector<TransportEvent>{}; })},
        {"pump_gathered",
         pumping([] { return one(transport_event::Gathered{description()}); })},
        {"pump_nominated",
         pumping([] { return one(transport_event::Nominated{}); })},
        {"pump_established",
         pumping([] { return one(transport_event::Established{}); })},
        {"pump_garbage", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(record("XXXX"));
           return items;
         })},
        {"pump_bad_control", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(record(control_prefix));
           return items;
         })},
        {"pump_input", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(input_record());
           return items;
         })},
        {"pump_bad_input", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(record(input_prefix, 2));
           return items;
         })},
        {"pump_feedback", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(feedback_record());
           return items;
         })},
        {"pump_media_ack", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(record(media_ack_prefix));
           return items;
         })},
        {"pump_datagram", pumping([] {
           return one(transport_event::Datagram{std::vector<std::byte>(4)});
         })},
        {"pump_failed",
         pumping([] { return one(transport_event::Failed{Error::timeout}); })},
        {"pump_failed_then_more", pumping([] {
           std::vector<TransportEvent> items;
           items.emplace_back(transport_event::Failed{Error::timeout});
           items.emplace_back(transport_event::Nominated{});
           return items;
         })},
        {"pump_error",
         [](Rig& rig) {
           rig.tally.pump_error = Error::crypto_unavailable;
           return events(rig.pump({}));
         }},
        {"send_control",
         [](Rig& rig) {
           return outcome(rig.link.send_control(
               control::Message{control::AdmissionPending{}}, 7, false, now));
         }},
        {"send_terminal",
         [](Rig& rig) {
           return outcome(rig.link.send_control(
               control::Message{control::End{control::Reason::user}}, 9, true,
               now));
         }},
        {"send_input",
         [](Rig& rig) {
           InputFrame frame;
           frame.binding = SeatBinding{1, 1};
           return outcome(rig.link.send_input(frame));
         }},
        {"send_feedback",
         [](Rig& rig) {
           FeedbackFrame frame;
           frame.binding = SeatBinding{1, 1};
           return outcome(rig.link.send_feedback(frame));
         }},
        {"send_media_ack",
         [](Rig& rig) {
           const std::vector<std::byte> bytes(4);
           return outcome(rig.link.send_media_ack(bytes));
         }},
        {"send_media",
         [](Rig& rig) {
           const std::vector<std::byte> bytes(4);
           return outcome(rig.link.send_media(bytes));
         }},
        {"media_key",
         [](Rig& rig) {
           const auto key = rig.link.media_key();
           return key ? std::string{"ok "}
                      : std::format("e{} ", static_cast<int>(key.error()));
         }},
        {"close",
         [](Rig& rig) {
           rig.link.close();
           return std::string{};
         }},
    };
  }

  struct Pin {
    std::string_view row;
    std::string_view column;
    std::string_view trace;
  };

  static auto pins() -> std::vector<Pin> {
    return {
        {"fresh", "gather", "ok |T r0 d0 c0"},
        {"fresh", "connect", "ok |T r0 d0 c0"},
        {"fresh", "pump_empty", "|T r0 d0 c0"},
        {"fresh", "pump_gathered", "0 |T r0 d0 c0"},
        {"fresh", "pump_nominated", "1 |T r0 d0 c0"},
        {"fresh", "pump_established", "10 f16 | r0 d0 c0"},
        {"fresh", "pump_garbage", "|T r0 d0 c0"},
        {"fresh", "pump_bad_control", "|T r0 d0 c0"},
        {"fresh", "pump_input", "|T r0 d0 c0"},
        {"fresh", "pump_bad_input", "|T r0 d0 c0"},
        {"fresh", "pump_feedback", "|T r0 d0 c0"},
        {"fresh", "pump_media_ack", "|T r0 d0 c0"},
        {"fresh", "pump_datagram", "|T r0 d0 c0"},
        {"fresh", "pump_failed", "10 f10 | r0 d0 c0"},
        {"fresh", "pump_failed_then_more", "10 f10 | r0 d0 c0"},
        {"fresh", "pump_error", "10 f15 | r0 d0 c0"},
        {"fresh", "send_control", "e6 |T r0 d0 c0"},
        {"fresh", "send_terminal", "e6 |T r0 d0 c0"},
        {"fresh", "send_input", "e6 |T r0 d0 c0"},
        {"fresh", "send_feedback", "e6 |T r0 d0 c0"},
        {"fresh", "send_media_ack", "e6 |T r0 d0 c0"},
        {"fresh", "send_media", "e6 |T r0 d0 c0"},
        {"fresh", "media_key", "e16 |T r0 d0 c0"},
        {"fresh", "close", "|C r0 d0 c1"},
        {"connected", "gather", "ok |T r0 d0 c0"},
        {"connected", "connect", "e16 |T r0 d0 c0"},
        {"connected", "pump_empty", "|T r0 d0 c0"},
        {"connected", "pump_gathered", "0 |T r0 d0 c0"},
        {"connected", "pump_nominated", "1 |T r0 d0 c0"},
        {"connected", "pump_established", "2 |ET r0 d0 c0"},
        {"connected", "pump_garbage", "|T r0 d0 c0"},
        {"connected", "pump_bad_control", "|T r0 d0 c0"},
        {"connected", "pump_input", "|T r0 d0 c0"},
        {"connected", "pump_bad_input", "|T r0 d0 c0"},
        {"connected", "pump_feedback", "|T r0 d0 c0"},
        {"connected", "pump_media_ack", "|T r0 d0 c0"},
        {"connected", "pump_datagram", "|T r0 d0 c0"},
        {"connected", "pump_failed", "10 f10 | r0 d0 c0"},
        {"connected", "pump_failed_then_more", "10 f10 | r0 d0 c0"},
        {"connected", "pump_error", "10 f15 | r0 d0 c0"},
        {"connected", "send_control", "e6 |T r0 d0 c0"},
        {"connected", "send_terminal", "e6 |T r0 d0 c0"},
        {"connected", "send_input", "e6 |T r0 d0 c0"},
        {"connected", "send_feedback", "e6 |T r0 d0 c0"},
        {"connected", "send_media_ack", "e6 |T r0 d0 c0"},
        {"connected", "send_media", "e6 |T r0 d0 c0"},
        {"connected", "media_key", "e16 |T r0 d0 c0"},
        {"connected", "close", "|C r0 d0 c1"},
        {"established", "gather", "ok |ET r0 d0 c0"},
        {"established", "connect", "e16 |ET r0 d0 c0"},
        {"established", "pump_empty", "|ET r0 d0 c0"},
        {"established", "pump_gathered", "0 |ET r0 d0 c0"},
        {"established", "pump_nominated", "1 |ET r0 d0 c0"},
        {"established", "pump_established", "10 f16 |E r0 d0 c0"},
        {"established", "pump_garbage", "9 m11 |ET r0 d0 c0"},
        {"established", "pump_bad_control", "9 m11 |ET r0 d0 c0"},
        {"established", "pump_input", "5 |ET r0 d0 c0"},
        {"established", "pump_bad_input", "9 m11 |ET r0 d0 c0"},
        {"established", "pump_feedback", "9 m11 |ET r0 d0 c0"},
        {"established", "pump_media_ack", "7 |ET r0 d0 c0"},
        {"established", "pump_datagram", "|ET r0 d0 c0"},
        {"established", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"established", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"established", "pump_error", "10 f15 |E r0 d0 c0"},
        {"established", "send_control", "ok |ET r0 d0 c0"},
        {"established", "send_terminal", "ok |ET r0 d0 c0"},
        {"established", "send_input", "e6 |ET r0 d0 c0"},
        {"established", "send_feedback", "ok |ET r1 d0 c0"},
        {"established", "send_media_ack", "e6 |ET r0 d0 c0"},
        {"established", "send_media", "ok |ET r0 d1 c0"},
        {"established", "media_key", "ok |ET r0 d0 c0"},
        {"established", "close", "|C r0 d0 c1"},
        {"queued", "gather", "ok |ET r0 d0 c0"},
        {"queued", "connect", "e16 |ET r0 d0 c0"},
        {"queued", "pump_empty", "|ET r1 d0 c0"},
        {"queued", "pump_gathered", "0 |ET r1 d0 c0"},
        {"queued", "pump_nominated", "1 |ET r1 d0 c0"},
        {"queued", "pump_established", "10 f16 |E r0 d0 c0"},
        {"queued", "pump_garbage", "9 m11 |ET r1 d0 c0"},
        {"queued", "pump_bad_control", "9 m11 |ET r1 d0 c0"},
        {"queued", "pump_input", "5 |ET r1 d0 c0"},
        {"queued", "pump_bad_input", "9 m11 |ET r1 d0 c0"},
        {"queued", "pump_feedback", "9 m11 |ET r1 d0 c0"},
        {"queued", "pump_media_ack", "7 |ET r1 d0 c0"},
        {"queued", "pump_datagram", "|ET r1 d0 c0"},
        {"queued", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"queued", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"queued", "pump_error", "10 f15 |E r0 d0 c0"},
        {"queued", "send_control", "ok |ET r0 d0 c0"},
        {"queued", "send_terminal", "ok |ET r0 d0 c0"},
        {"queued", "send_input", "e6 |ET r0 d0 c0"},
        {"queued", "send_feedback", "ok |ET r1 d0 c0"},
        {"queued", "send_media_ack", "e6 |ET r0 d0 c0"},
        {"queued", "send_media", "ok |ET r0 d1 c0"},
        {"queued", "media_key", "ok |ET r0 d0 c0"},
        {"queued", "close", "|C r0 d0 c1"},
        {"terminal", "gather", "ok |ET r0 d0 c0"},
        {"terminal", "connect", "e16 |ET r0 d0 c0"},
        {"terminal", "pump_empty", "|ET r1 d0 c0"},
        {"terminal", "pump_gathered", "0 |ET r1 d0 c0"},
        {"terminal", "pump_nominated", "1 |ET r1 d0 c0"},
        {"terminal", "pump_established", "10 f16 |E r0 d0 c0"},
        {"terminal", "pump_garbage", "9 m11 |ET r1 d0 c0"},
        {"terminal", "pump_bad_control", "9 m11 |ET r1 d0 c0"},
        {"terminal", "pump_input", "5 |ET r1 d0 c0"},
        {"terminal", "pump_bad_input", "9 m11 |ET r1 d0 c0"},
        {"terminal", "pump_feedback", "9 m11 |ET r1 d0 c0"},
        {"terminal", "pump_media_ack", "7 |ET r1 d0 c0"},
        {"terminal", "pump_datagram", "|ET r1 d0 c0"},
        {"terminal", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"terminal", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"terminal", "pump_error", "10 f15 |E r0 d0 c0"},
        {"terminal", "send_control", "e11 |ET r0 d0 c0"},
        {"terminal", "send_terminal", "e6 |ET r0 d0 c0"},
        {"terminal", "send_input", "e6 |ET r0 d0 c0"},
        {"terminal", "send_feedback", "ok |ET r1 d0 c0"},
        {"terminal", "send_media_ack", "e6 |ET r0 d0 c0"},
        {"terminal", "send_media", "ok |ET r0 d1 c0"},
        {"terminal", "media_key", "ok |ET r0 d0 c0"},
        {"terminal", "close", "|C r0 d0 c1"},
        {"deaf", "gather", "ok |ET r0 d0 c0"},
        {"deaf", "connect", "e16 |ET r0 d0 c0"},
        {"deaf", "pump_empty", "|ET r0 d0 c0"},
        {"deaf", "pump_gathered", "0 |ET r0 d0 c0"},
        {"deaf", "pump_nominated", "1 |ET r0 d0 c0"},
        {"deaf", "pump_established", "10 f16 |E r0 d0 c0"},
        {"deaf", "pump_garbage", "|ET r0 d0 c0"},
        {"deaf", "pump_bad_control", "|ET r0 d0 c0"},
        {"deaf", "pump_input", "|ET r0 d0 c0"},
        {"deaf", "pump_bad_input", "|ET r0 d0 c0"},
        {"deaf", "pump_feedback", "|ET r0 d0 c0"},
        {"deaf", "pump_media_ack", "|ET r0 d0 c0"},
        {"deaf", "pump_datagram", "|ET r0 d0 c0"},
        {"deaf", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"deaf", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"deaf", "pump_error", "10 f15 |E r0 d0 c0"},
        {"deaf", "send_control", "ok |ET r0 d0 c0"},
        {"deaf", "send_terminal", "ok |ET r0 d0 c0"},
        {"deaf", "send_input", "e6 |ET r0 d0 c0"},
        {"deaf", "send_feedback", "ok |ET r1 d0 c0"},
        {"deaf", "send_media_ack", "e6 |ET r0 d0 c0"},
        {"deaf", "send_media", "ok |ET r0 d1 c0"},
        {"deaf", "media_key", "ok |ET r0 d0 c0"},
        {"deaf", "close", "|C r0 d0 c1"},
        {"stopped", "gather", "ok |E r0 d0 c0"},
        {"stopped", "connect", "e16 |E r0 d0 c0"},
        {"stopped", "pump_empty", "|E r0 d0 c0"},
        {"stopped", "pump_gathered", "|E r0 d0 c0"},
        {"stopped", "pump_nominated", "|E r0 d0 c0"},
        {"stopped", "pump_established", "|E r0 d0 c0"},
        {"stopped", "pump_garbage", "|E r0 d0 c0"},
        {"stopped", "pump_bad_control", "|E r0 d0 c0"},
        {"stopped", "pump_input", "|E r0 d0 c0"},
        {"stopped", "pump_bad_input", "|E r0 d0 c0"},
        {"stopped", "pump_feedback", "|E r0 d0 c0"},
        {"stopped", "pump_media_ack", "|E r0 d0 c0"},
        {"stopped", "pump_datagram", "|E r0 d0 c0"},
        {"stopped", "pump_failed", "|E r0 d0 c0"},
        {"stopped", "pump_failed_then_more", "|E r0 d0 c0"},
        {"stopped", "pump_error", "|E r0 d0 c0"},
        {"stopped", "send_control", "e6 |E r0 d0 c0"},
        {"stopped", "send_terminal", "e6 |E r0 d0 c0"},
        {"stopped", "send_input", "e6 |E r0 d0 c0"},
        {"stopped", "send_feedback", "e6 |E r0 d0 c0"},
        {"stopped", "send_media_ack", "e6 |E r0 d0 c0"},
        {"stopped", "send_media", "e6 |E r0 d0 c0"},
        {"stopped", "media_key", "ok |E r0 d0 c0"},
        {"stopped", "close", "|C r0 d0 c1"},
        {"closed", "gather", "e16 |C r0 d0 c1"},
        {"closed", "connect", "e16 |C r0 d0 c1"},
        {"closed", "pump_empty", "|C r0 d0 c1"},
        {"closed", "pump_gathered", "|C r0 d0 c1"},
        {"closed", "pump_nominated", "|C r0 d0 c1"},
        {"closed", "pump_established", "|C r0 d0 c1"},
        {"closed", "pump_garbage", "|C r0 d0 c1"},
        {"closed", "pump_bad_control", "|C r0 d0 c1"},
        {"closed", "pump_input", "|C r0 d0 c1"},
        {"closed", "pump_bad_input", "|C r0 d0 c1"},
        {"closed", "pump_feedback", "|C r0 d0 c1"},
        {"closed", "pump_media_ack", "|C r0 d0 c1"},
        {"closed", "pump_datagram", "|C r0 d0 c1"},
        {"closed", "pump_failed", "|C r0 d0 c1"},
        {"closed", "pump_failed_then_more", "|C r0 d0 c1"},
        {"closed", "pump_error", "|C r0 d0 c1"},
        {"closed", "send_control", "e6 |C r0 d0 c1"},
        {"closed", "send_terminal", "e6 |C r0 d0 c1"},
        {"closed", "send_input", "e6 |C r0 d0 c1"},
        {"closed", "send_feedback", "e6 |C r0 d0 c1"},
        {"closed", "send_media_ack", "e6 |C r0 d0 c1"},
        {"closed", "send_media", "e6 |C r0 d0 c1"},
        {"closed", "media_key", "e16 |C r0 d0 c1"},
        {"closed", "close", "|C r0 d0 c1"},
        {"stopped_closed", "gather", "e16 |C r0 d0 c1"},
        {"stopped_closed", "connect", "e16 |C r0 d0 c1"},
        {"stopped_closed", "pump_empty", "|C r0 d0 c1"},
        {"stopped_closed", "pump_gathered", "|C r0 d0 c1"},
        {"stopped_closed", "pump_nominated", "|C r0 d0 c1"},
        {"stopped_closed", "pump_established", "|C r0 d0 c1"},
        {"stopped_closed", "pump_garbage", "|C r0 d0 c1"},
        {"stopped_closed", "pump_bad_control", "|C r0 d0 c1"},
        {"stopped_closed", "pump_input", "|C r0 d0 c1"},
        {"stopped_closed", "pump_bad_input", "|C r0 d0 c1"},
        {"stopped_closed", "pump_feedback", "|C r0 d0 c1"},
        {"stopped_closed", "pump_media_ack", "|C r0 d0 c1"},
        {"stopped_closed", "pump_datagram", "|C r0 d0 c1"},
        {"stopped_closed", "pump_failed", "|C r0 d0 c1"},
        {"stopped_closed", "pump_failed_then_more", "|C r0 d0 c1"},
        {"stopped_closed", "pump_error", "|C r0 d0 c1"},
        {"stopped_closed", "send_control", "e6 |C r0 d0 c1"},
        {"stopped_closed", "send_terminal", "e6 |C r0 d0 c1"},
        {"stopped_closed", "send_input", "e6 |C r0 d0 c1"},
        {"stopped_closed", "send_feedback", "e6 |C r0 d0 c1"},
        {"stopped_closed", "send_media_ack", "e6 |C r0 d0 c1"},
        {"stopped_closed", "send_media", "e6 |C r0 d0 c1"},
        {"stopped_closed", "media_key", "e16 |C r0 d0 c1"},
        {"stopped_closed", "close", "|C r0 d0 c1"},
        {"deaf_stopped", "gather", "ok |E r0 d0 c0"},
        {"deaf_stopped", "connect", "e16 |E r0 d0 c0"},
        {"deaf_stopped", "pump_empty", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_gathered", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_nominated", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_established", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_garbage", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_bad_control", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_input", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_bad_input", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_feedback", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_media_ack", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_datagram", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_failed", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_failed_then_more", "|E r0 d0 c0"},
        {"deaf_stopped", "pump_error", "|E r0 d0 c0"},
        {"deaf_stopped", "send_control", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "send_terminal", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "send_input", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "send_feedback", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "send_media_ack", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "send_media", "e6 |E r0 d0 c0"},
        {"deaf_stopped", "media_key", "ok |E r0 d0 c0"},
        {"deaf_stopped", "close", "|C r0 d0 c1"},
        {"guest_established", "gather", "ok |ET r0 d0 c0"},
        {"guest_established", "connect", "e16 |ET r0 d0 c0"},
        {"guest_established", "pump_empty", "|ET r0 d0 c0"},
        {"guest_established", "pump_gathered", "0 |ET r0 d0 c0"},
        {"guest_established", "pump_nominated", "1 |ET r0 d0 c0"},
        {"guest_established", "pump_established", "10 f16 |E r0 d0 c0"},
        {"guest_established", "pump_garbage", "9 m11 |ET r0 d0 c0"},
        {"guest_established", "pump_bad_control", "9 m11 |ET r0 d0 c0"},
        {"guest_established", "pump_input", "9 m11 |ET r0 d0 c0"},
        {"guest_established", "pump_bad_input", "9 m11 |ET r0 d0 c0"},
        {"guest_established", "pump_feedback", "6 |ET r0 d0 c0"},
        {"guest_established", "pump_media_ack", "9 m11 |ET r0 d0 c0"},
        {"guest_established", "pump_datagram", "8 |ET r0 d0 c0"},
        {"guest_established", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"guest_established", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"guest_established", "pump_error", "10 f15 |E r0 d0 c0"},
        {"guest_established", "send_control", "ok |ET r0 d0 c0"},
        {"guest_established", "send_terminal", "ok |ET r0 d0 c0"},
        {"guest_established", "send_input", "ok |ET r1 d0 c0"},
        {"guest_established", "send_feedback", "e6 |ET r0 d0 c0"},
        {"guest_established", "send_media_ack", "ok |ET r1 d0 c0"},
        {"guest_established", "send_media", "e6 |ET r0 d0 c0"},
        {"guest_established", "media_key", "ok |ET r0 d0 c0"},
        {"guest_established", "close", "|C r0 d0 c1"},
        {"guest_deaf", "gather", "ok |ET r0 d0 c0"},
        {"guest_deaf", "connect", "e16 |ET r0 d0 c0"},
        {"guest_deaf", "pump_empty", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_gathered", "0 |ET r0 d0 c0"},
        {"guest_deaf", "pump_nominated", "1 |ET r0 d0 c0"},
        {"guest_deaf", "pump_established", "10 f16 |E r0 d0 c0"},
        {"guest_deaf", "pump_garbage", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_bad_control", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_input", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_bad_input", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_feedback", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_media_ack", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_datagram", "|ET r0 d0 c0"},
        {"guest_deaf", "pump_failed", "10 f10 |E r0 d0 c0"},
        {"guest_deaf", "pump_failed_then_more", "10 f10 |E r0 d0 c0"},
        {"guest_deaf", "pump_error", "10 f15 |E r0 d0 c0"},
        {"guest_deaf", "send_control", "ok |ET r0 d0 c0"},
        {"guest_deaf", "send_terminal", "ok |ET r0 d0 c0"},
        {"guest_deaf", "send_input", "ok |ET r1 d0 c0"},
        {"guest_deaf", "send_feedback", "e6 |ET r0 d0 c0"},
        {"guest_deaf", "send_media_ack", "ok |ET r1 d0 c0"},
        {"guest_deaf", "send_media", "e6 |ET r0 d0 c0"},
        {"guest_deaf", "media_key", "ok |ET r0 d0 c0"},
        {"guest_deaf", "close", "|C r0 d0 c1"},
        {"guest_stopped", "gather", "ok |E r0 d0 c0"},
        {"guest_stopped", "connect", "e16 |E r0 d0 c0"},
        {"guest_stopped", "pump_empty", "|E r0 d0 c0"},
        {"guest_stopped", "pump_gathered", "|E r0 d0 c0"},
        {"guest_stopped", "pump_nominated", "|E r0 d0 c0"},
        {"guest_stopped", "pump_established", "|E r0 d0 c0"},
        {"guest_stopped", "pump_garbage", "|E r0 d0 c0"},
        {"guest_stopped", "pump_bad_control", "|E r0 d0 c0"},
        {"guest_stopped", "pump_input", "|E r0 d0 c0"},
        {"guest_stopped", "pump_bad_input", "|E r0 d0 c0"},
        {"guest_stopped", "pump_feedback", "|E r0 d0 c0"},
        {"guest_stopped", "pump_media_ack", "|E r0 d0 c0"},
        {"guest_stopped", "pump_datagram", "|E r0 d0 c0"},
        {"guest_stopped", "pump_failed", "|E r0 d0 c0"},
        {"guest_stopped", "pump_failed_then_more", "|E r0 d0 c0"},
        {"guest_stopped", "pump_error", "|E r0 d0 c0"},
        {"guest_stopped", "send_control", "e6 |E r0 d0 c0"},
        {"guest_stopped", "send_terminal", "e6 |E r0 d0 c0"},
        {"guest_stopped", "send_input", "e6 |E r0 d0 c0"},
        {"guest_stopped", "send_feedback", "e6 |E r0 d0 c0"},
        {"guest_stopped", "send_media_ack", "e6 |E r0 d0 c0"},
        {"guest_stopped", "send_media", "e6 |E r0 d0 c0"},
        {"guest_stopped", "media_key", "ok |E r0 d0 c0"},
        {"guest_stopped", "close", "|C r0 d0 c1"},
    };
  }
};

TEST_F(LinkTransitions, EveryCallInEveryStateDoesWhatTheTableSays) {
  const auto table = pins();
  for (const auto& row : rows()) {
    for (const auto& [column, step] : columns()) {
      Rig rig{row.role};
      for (const auto& setup : row.setup) static_cast<void>(setup(rig));
      const auto trace = step(rig) + state(rig);
      const auto pin = std::ranges::find_if(table, [&](const Pin& item) {
        return item.row == row.name && item.column == column;
      });
      if (pin == table.end()) {
        ADD_FAILURE() << "{\"" << row.name << "\", \"" << column << "\", \""
                      << trace << "\"},";
        continue;
      }
      EXPECT_EQ(trace, pin->trace) << row.name << " " << column;
    }
  }
}

}  // namespace
