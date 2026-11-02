#pragma once

#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"
#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/net/kakusan/link.hpp"
#include "src/core/net/kakusan/media_receiver.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test::session_core {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
namespace control = libchiaking::net::kakusan::control;
namespace link_event = libchiaking::net::kakusan::link_event;
using libchiaking::net::kakusan::Button;
using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::TransportRole;

class CoreValues {
 public:
  static constexpr std::uint64_t one_second = 1'000'000;
  static constexpr std::uint64_t start_now = 1'000'000;
  static constexpr std::uint64_t start_wall = 1'791'115'200;
  static constexpr std::uint8_t host_tag = 0x10;
  static constexpr std::string_view profile_header =
      "000000016764002aacd9405005bb01";
  static constexpr std::string_view audio_header =
      "02100000bb80000003c000000001";

  struct BuiltReply {
    std::string block;
    Transcript transcript{};
    SealedInvitation invitation;
  };

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto identity(std::uint8_t tag) -> DeviceIdentity {
    return Identities::tagged(crypto(), tag);
  }

  static auto source_keys(std::byte difference = {})
      -> libchiaking::play::ReceiveKeys {
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> key;
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> iv;
    for (std::size_t index = 0; index < key.size(); ++index) {
      key[index] = static_cast<std::byte>(0x30 + index);
      iv[index] = static_cast<std::byte>(0x70 + index);
    }
    key[0] ^= difference;
    return libchiaking::play::ReceiveKeys{std::move(key), std::move(iv)};
  }

  static auto pad_seat(std::uint8_t index)
      -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
  }

  static auto stream_info() -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription info;
    info.audio.channels = 2;
    info.audio.bits = 16;
    info.audio.rate = 48000;
    info.audio.frame_size = 960;
    info.profiles.push_back(libchiaking::media::VideoProfile{
        1280, 720, test_support::Bytes::unhex(profile_header)});
    info.audio_header = test_support::Bytes::unhex_array<14>(audio_header);
    return info;
  }

  static auto video_datagram(std::uint64_t position, std::size_t size,
                             std::uint8_t profile = 0)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(size);
    for (std::size_t offset = 0; offset < size; ++offset)
      bytes[offset] =
          static_cast<std::byte>((position * 13 + offset * 7) & 0xffU);
    bytes[0] = std::byte{0x02};
    put_u32(bytes, 5, 0);
    put_u32(bytes, 14, low_bits(position));
    bytes[20] = static_cast<std::byte>(profile << 5U);
    return bytes;
  }

  static auto haptics_datagram(std::uint64_t position)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes(24, std::byte{0x77});
    bytes[0] = std::byte{0x03};
    put_u32(bytes, 5, (4U << 8U) | 1U);
    bytes[9] = std::byte{5};
    put_u32(bytes, 14, low_bits(position));
    bytes[19] = std::byte{2};
    return bytes;
  }

  static auto text(const Code& code) -> std::string {
    return std::string{code.data(), code.size()};
  }

  static auto reply_seed(std::uint8_t tag) -> BlockSeed {
    BlockSeed seed{};
    seed.fill(static_cast<std::byte>(tag));
    return seed;
  }

  static auto build_reply(const std::string& invitation_block,
                          const DeviceIdentity& identity,
                          const TransportDescription& local, std::uint8_t nonce)
      -> std::optional<BuiltReply> {
    auto invitation =
        ShortBlockCodec::parse_invitation(crypto(), invitation_block);
    EXPECT_TRUE(invitation);
    if (!invitation) return std::nullopt;
    const ShortReply reply{
        .seed = reply_seed(nonce),
        .invite_tag = ShortBlockCodec::tag_of(invitation.value().hash),
        .dtls_sha256 = local.certificate_digest,
        .identity_hint = ShortBlockCodec::hint_of(identity.key().bytes()),
        .ice = local.ice.clone(),
        .reply_nonce = {}};
    const auto sealed = ShortBlockCodec::seal(crypto(), reply);
    EXPECT_TRUE(sealed);
    if (!sealed) return std::nullopt;
    const auto transcript = Exchange::transcript(
        crypto(), invitation.value().hash, sealed.value().hash);
    EXPECT_TRUE(transcript);
    if (!transcript) return std::nullopt;
    return BuiltReply{std::string{sealed.value().text.view()},
                      transcript.value(), std::move(invitation).value()};
  }

  static auto mapped(const InputState& state)
      -> libchiaking::controllers::ControllerState {
    const auto expected = ControllerMap::to_source_state(state, button_mask);
    EXPECT_TRUE(expected);
    if (!expected) return {};
    return expected.value();
  }

  static auto same_input(const libchiaking::controllers::ControllerState& left,
                         const libchiaking::controllers::ControllerState& right)
      -> ::testing::AssertionResult {
    if (left.buttons == right.buttons && left.l2 == right.l2 &&
        left.r2 == right.r2 && left.left_x == right.left_x &&
        left.left_y == right.left_y && left.right_x == right.right_x &&
        left.right_y == right.right_y &&
        left.touches[0].id == right.touches[0].id)
      return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "buttons " << left.buttons << " and " << right.buttons
           << " or the axes, triggers or touch differ";
  }

  static auto is_neutral(const libchiaking::controllers::ControllerState& state)
      -> bool {
    return static_cast<bool>(
        same_input(state, libchiaking::controllers::ControllerState{}));
  }

  static auto button(Button value) -> std::uint32_t {
    return static_cast<std::uint32_t>(value);
  }

  static auto pressing(Button value) -> InputState {
    return InputState{button(value), 0, 0, 0, 0, 0, 0, {}};
  }

  static auto is_media_ack(const std::vector<std::byte>& bytes) -> bool {
    return bytes.size() >= 4 && bytes[0] == std::byte{'R'} &&
           bytes[1] == std::byte{'C'} && bytes[2] == std::byte{'A'} &&
           bytes[3] == std::byte{'1'};
  }

 private:
  static void put_u32(std::vector<std::byte>& bytes, std::size_t offset,
                      std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      bytes[offset + index] =
          static_cast<std::byte>((value >> (24U - 8U * index)) & 0xffU);
  }

  static auto low_bits(std::uint64_t position) -> std::uint32_t {
    return static_cast<std::uint32_t>(position & 0xffffffffU);
  }
};

class CoreGuest : public CoreValues {
 public:
  CoreGuest(std::shared_ptr<test_support::kakusan::LoopbackLink> shared,
            std::uint8_t identity_tag)
      : wire_link(std::move(shared)),
        identity(CoreValues::identity(identity_tag)),
        entropy(0x5000U + identity_tag),
        tag(identity_tag),
        link(std::make_unique<test_support::kakusan::LoopbackTransport>(
                 wire_link, TransportRole::guest),
             TransportRole::guest) {
    wire_link->attached[1] = true;
  }

  void apply(VisitingPlayer::Effects effects) {
    for (auto& effect : effects) {
      if (const auto* send = std::get_if<SendControl>(&effect)) {
        static_cast<void>(
            link.send_control(send->message, send->token, send->terminal, now));
      } else if (auto* configure = std::get_if<ConfigureReceiver>(&effect)) {
        EXPECT_TRUE(configure->setup.validate());
        source_session = configure->setup.source_session_id;
        source_protocol = configure->setup.source_protocol;
        receive_key = configure->setup.receive_key.clone();
        receive_iv = configure->setup.receive_iv.clone();
        apply(spoke->handle(ReceiverReady{now}));
      } else if (std::holds_alternative<PrepareController>(effect)) {
        apply(spoke->handle(ControllerReady{now}));
      } else if (const auto* input = std::get_if<SendInput>(&effect)) {
        static_cast<void>(link.send_input(input->frame));
      } else if (std::holds_alternative<StartMediaReception>(effect)) {
        auto key = link.media_key();
        ASSERT_TRUE(key);
        ASSERT_TRUE(transcript);
        const auto connection =
            TransportFrameCodec::connection_id(crypto(), *transcript);
        ASSERT_TRUE(connection);
        receiver = std::make_unique<MediaReceiver>(
            crypto(), std::move(key).value(), connection.value());
      } else if (std::holds_alternative<StopMediaReception>(effect)) {
        receiver.reset();
      } else if (const auto* rumble = std::get_if<ApplyRumble>(&effect)) {
        rumbles.push_back(*rumble);
      } else if (const auto* display = std::get_if<DisplayCode>(&effect)) {
        code = display->code.clone();
      } else if (std::holds_alternative<CloseTransport>(effect)) {
        ++closed;
        link.close();
        receiver.reset();
      } else if (const auto* shown = std::get_if<ShowState>(&effect)) {
        if (shown->error) error = shown->error;
        if (shown->reason) reason = shown->reason;
        by_peer = by_peer || shown->by_peer;
      }
    }
  }

  void deliver(std::span<LinkEvent> events) {
    for (auto& event : events) {
      if (auto* gathered = std::get_if<link_event::Gathered>(&event)) {
        description = std::move(gathered->local);
        continue;
      }
      if (!spoke) continue;
      if (std::holds_alternative<link_event::Nominated>(event)) {
        apply(spoke->handle(IceNominated{now}));
      } else if (const auto* established =
                     std::get_if<link_event::Established>(&event)) {
        apply(spoke->handle(DtlsEstablished{
            now, established->exporter.clone(),
            IdentityProof::sign(crypto(), Role::guest, *transcript,
                                established->exporter, identity.saved().seed,
                                std::nullopt)
                .value()}));
      } else if (auto* message = std::get_if<link_event::Control>(&event)) {
        apply(spoke->handle(ControlReceived{now, std::move(message->message)}));
      } else if (const auto* acked = std::get_if<link_event::Acked>(&event)) {
        apply(spoke->handle(ControlAcked{now, acked->token}));
      } else if (const auto* feedback =
                     std::get_if<link_event::Feedback>(&event)) {
        apply(spoke->handle(FeedbackReceived{now, feedback->frame}));
      } else if (const auto* media = std::get_if<link_event::Media>(&event)) {
        if (!receiver) continue;
        auto received = receiver->receive(media->datagram, now);
        ASSERT_TRUE(received);
        if (received.value()) packets.push_back(std::move(*received.value()));
      } else if (const auto* malformed_event =
                     std::get_if<link_event::Malformed>(&event)) {
        ++malformed;
        apply(spoke->handle(MalformedData{now, malformed_event->error}));
      } else if (std::holds_alternative<link_event::Failed>(event)) {
        apply(spoke->handle(TransportFailed{now}));
      }
    }
  }

  void step(std::uint64_t at) {
    now = at;
    auto events = link.pump(now);
    deliver(events);
    if (spoke) apply(spoke->on_time(now));
    if (receiver) {
      while (auto ack = receiver->poll(now))
        static_cast<void>(link.send_media_ack(*ack));
    }
  }

  [[nodiscard]] auto next_deadline() const -> std::optional<std::uint64_t> {
    auto deadline = link.next_deadline_us();
    if (spoke)
      deadline = libchiaking::timing::Deadline::earliest(
          deadline, spoke->next_deadline_us());
    if (receiver)
      deadline = libchiaking::timing::Deadline::earliest(
          deadline, receiver->next_deadline_us());
    return deadline;
  }

  auto answer(const std::string& invitation_block) -> std::string {
    auto built = build_reply(invitation_block, identity, *description, tag);
    if (!built) return {};
    const auto& offered = built->invitation.body;
    transcript = built->transcript;
    PairingContext context;
    context.transcript = built->transcript;
    context.parties.guest_identity = identity.key().bytes();
    context.invitation_id = offered.invitation_id;
    context.peer_hint = offered.identity_hint;
    spoke = std::make_unique<VisitingPlayer>(crypto(), entropy, context,
                                             offered.source_protocol);
    EXPECT_TRUE(link.connect(
        TransportDescription{offered.ice.clone(), offered.dtls_sha256},
        built->transcript, now));
    reply_block = built->block;
    return built->block;
  }

  void sample(const InputState& state) {
    apply(spoke->handle(ControllerSampled{now, state}));
  }

  void leave() { apply(spoke->handle(Cancel{now, control::Reason::user})); }

  auto state() const -> Progress {
    return spoke ? spoke->state() : Progress::waiting;
  }

  std::shared_ptr<test_support::kakusan::LoopbackLink> wire_link;
  DeviceIdentity identity;
  test_support::SeededRandom entropy;
  std::uint8_t tag{};
  Link link;
  std::uint64_t now{};
  std::optional<TransportDescription> description;
  std::optional<Transcript> transcript;
  std::unique_ptr<VisitingPlayer> spoke;
  std::unique_ptr<MediaReceiver> receiver;
  std::string reply_block;
  std::optional<Code> code;
  std::optional<control::SourceSessionId> source_session;
  std::optional<control::SourceKey> receive_key;
  std::optional<control::SourceKey> receive_iv;
  std::uint8_t source_protocol{};
  std::vector<SourcePacket> packets;
  std::vector<ApplyRumble> rumbles;
  std::optional<Error> error;
  std::optional<control::Reason> reason;
  bool by_peer{};
  std::size_t closed{};
  std::size_t malformed{};
};

struct Party {
  InvitationId id{};
  CoreGuest* guest{};
};

class CoreRig : public CoreValues {
 public:
  using Core = SharedSession::Core;

  explicit CoreRig(SharingRules config = *SharingRules::create(4, 1),
                   bool with_source = true) {
    auto created =
        Core::create(Core::Dependencies{crypto(), entropy, factory,
                                        test_support::process_threading()},
                     identity(host_tag), config);
    EXPECT_TRUE(created);
    if (!created) return;
    session = std::move(created).value();
    if (with_source) start_source();
  }

  auto clock() const -> Clock {
    return Clock{now,
                 start_wall + (now - start_now) / one_second + wall_offset};
  }

  void start_source() {
    session->source_keys(12, source_keys());
    EXPECT_TRUE(session->source_started(stream_info(), 60, now));
    drain();
  }

  void drain() {
    while (auto output = session->take_output()) {
      if (const auto* code = std::get_if<Core::CodeReady>(&*output))
        host_codes.insert_or_assign(code->guest, code->code.clone());
      if (const auto* join = std::get_if<Core::JoinRequested>(&*output)) {
        const auto pad = pads.find(join->guest);
        if (auto_join && pad != pads.end())
          EXPECT_TRUE(
              session->source_joined(join->seat, pad_seat(pad->second), now));
        else
          held_joins.push_back(*join);
      }
      outputs.push_back(std::move(*output));
    }
  }

  void step(std::uint64_t advance = 1000) {
    now += advance;
    session->pump(clock());
    drain();
    for (auto& guest : guests) guest->step(now);
  }

  void run(std::size_t steps, std::uint64_t advance = 1000) {
    for (std::size_t index = 0; index < steps; ++index) step(advance);
  }

  template <class Done>
  auto run_until(Done done, std::size_t limit = 20000,
                 std::uint64_t advance = 1000) -> ::testing::AssertionResult {
    for (std::size_t index = 0; index < limit && !done(); ++index)
      step(advance);
    if (done()) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "not reached within " << limit << " steps";
  }

  template <class Output>
  auto all(std::size_t from = 0) const -> std::vector<Output> {
    std::vector<Output> found;
    for (std::size_t index = from; index < outputs.size(); ++index) {
      if (const auto* output = std::get_if<Output>(&outputs[index])) {
        if constexpr (std::is_same_v<Output, Core::CodeReady>)
          found.push_back({output->guest, output->code.clone()});
        else if constexpr (std::is_same_v<Output, Core::InvitationReady>)
          found.push_back(
              {output->guest, output->block.clone(), output->expires_at});
        else
          found.push_back(*output);
      }
    }
    return found;
  }

  template <class Output>
  auto count(std::size_t from = 0) const -> std::size_t {
    std::size_t found = 0;
    for (std::size_t index = from; index < outputs.size(); ++index)
      found += std::holds_alternative<Output>(outputs[index]) ? 1U : 0U;
    return found;
  }

  [[nodiscard]] auto last_state(const InvitationId& id) const
      -> std::optional<Core::GuestState> {
    std::optional<Core::GuestState> found;
    for (const auto& state : all<Core::GuestState>()) {
      if (state.guest == id) found = state;
    }
    return found;
  }

  auto host_state(const InvitationId& id) const -> Progress {
    const auto state = last_state(id);
    return state ? state->state : Progress::waiting;
  }

  auto inputs(std::uint8_t pad, std::size_t from = 0) const
      -> std::vector<libchiaking::controllers::ControllerState> {
    std::vector<libchiaking::controllers::ControllerState> found;
    for (const auto& input : all<Core::ControllerInput>(from)) {
      const auto owner = pads.find(input.guest);
      if (owner != pads.end() && owner->second == pad)
        found.push_back(input.state);
    }
    return found;
  }

  auto removals(const InvitationId& id, std::size_t from = 0) const
      -> std::vector<Core::RemovalRequested> {
    std::vector<Core::RemovalRequested> found;
    for (const auto& removal : all<Core::RemovalRequested>(from)) {
      if (removal.guest == id) found.push_back(removal);
    }
    return found;
  }

  [[nodiscard]] auto block_for(const InvitationId& id) const
      -> std::optional<std::string> {
    for (const auto& ready : all<Core::InvitationReady>()) {
      if (ready.guest == id) return std::string{ready.block.view()};
    }
    return std::nullopt;
  }

  void invite_guest(std::uint8_t tag) {
    const auto id = session->invite(clock());
    ASSERT_TRUE(id);
    guests.push_back(std::make_unique<CoreGuest>(exchange->links.back(), tag));
    auto& guest = *guests.back();
    guest.now = now;
    ASSERT_TRUE(
        guest.link.gather(now, ShortBlockCodec::credentials(
                                   crypto(), reply_seed(tag), BlockKind::reply)
                                   .value()));
    ASSERT_TRUE(
        run_until([&] { return block_for(id.value()) && guest.description; }));
    party = Party{id.value(), &guest};
  }

  void reply(const Party& replying) {
    const auto block = replying.guest->answer(*block_for(replying.id));
    const auto imported = session->import_reply(block, clock());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value(), replying.id);
    drain();
  }

  void verify(const Party& verifying) {
    ASSERT_TRUE(run_until([&] {
      return verifying.guest->state() == Progress::verifying &&
             host_codes.contains(verifying.id);
    }));
    const auto value =
        static_cast<std::uint16_t>(1000U + 37U * verifying.guest->tag);
    verifying.guest->apply(verifying.guest->spoke->handle(
        LocalCode{now, Code::from_value(value).value()}));
    ASSERT_TRUE(verifying.guest->code);
  }

  void exchange_codes(const Party& exchanging, bool correct = true) {
    auto guest_code = text(*exchanging.guest->code);
    if (!correct)
      guest_code[3] =
          guest_code[3] == '9' ? '0' : static_cast<char>(guest_code[3] + 1);
    EXPECT_TRUE(session->enter_code(exchanging.id, guest_code, now));
    drain();
    exchanging.guest->apply(exchanging.guest->spoke->handle(
        PeerCodeEntered{now, host_codes.at(exchanging.id).clone()}));
  }

  void authenticate(std::uint8_t tag, std::uint8_t pad) {
    ASSERT_NO_FATAL_FAILURE(invite_guest(tag));
    const auto authenticating = party;
    pads[authenticating.id] = pad;
    ASSERT_NO_FATAL_FAILURE(reply(authenticating));
    ASSERT_NO_FATAL_FAILURE(verify(authenticating));
    ASSERT_NO_FATAL_FAILURE(exchange_codes(authenticating));
    ASSERT_TRUE(run_until([&] {
      return authenticating.guest->state() == Progress::waiting_for_seat &&
             host_state(authenticating.id) == Progress::waiting_for_seat;
    }));
  }

  void admit(std::uint8_t tag, std::uint8_t pad) {
    ASSERT_NO_FATAL_FAILURE(authenticate(tag, pad));
    const auto admitting = party;
    EXPECT_TRUE(session->approve(admitting.id, now));
    drain();
    ASSERT_TRUE(run_until([&] {
      return admitting.guest->state() == Progress::playing &&
             host_state(admitting.id) == Progress::playing &&
             admitting.guest->receiver;
    }));
    run(10);
  }

  void send(libchiaking::play::SharedDatagram::Media media,
            std::uint64_t position, const std::vector<std::byte>& bytes) {
    session->source_datagram(
        SourceDescriptions::describe(12, media, position, bytes));
  }

  void send_video(std::size_t count, std::size_t size = 900,
                  std::size_t spread = 2400) {
    for (std::size_t index = 0; index < count; ++index) {
      const auto position = next_position;
      next_position += 4096;
      auto bytes =
          video_datagram(position, size + (index * 701) % (spread + 1));
      send(libchiaking::play::SharedDatagram::Media::video, position, bytes);
      sent.emplace(position, std::move(bytes));
    }
  }

  void check_packets(const CoreGuest& guest) const {
    for (const auto& packet : guest.packets) {
      EXPECT_EQ(packet.kind, MediaKind::video);
      const auto found = sent.find(packet.key_position);
      ASSERT_NE(found, sent.end());
      EXPECT_EQ(found->second, packet.bytes);
    }
  }

  std::shared_ptr<test_support::kakusan::LoopbackExchange> exchange{
      std::make_shared<test_support::kakusan::LoopbackExchange>(crypto())};
  test_support::kakusan::LoopbackFactory factory{exchange};
  test_support::SeededRandom entropy{0x1234};
  std::unique_ptr<Core> session;
  std::uint64_t now{start_now};
  std::vector<Core::Output> outputs;
  std::map<InvitationId, Code> host_codes;
  std::map<InvitationId, std::uint8_t> pads;
  std::vector<Core::JoinRequested> held_joins;
  bool auto_join{true};
  std::vector<std::unique_ptr<CoreGuest>> guests;
  std::map<std::uint64_t, std::vector<std::byte>> sent;
  std::uint64_t next_position{44992};
  std::uint64_t wall_offset{};
  Party party;
};

class SharedSessionCoreTest : public ::testing::Test, protected CoreValues {};

}  // namespace kakusan_test::session_core
