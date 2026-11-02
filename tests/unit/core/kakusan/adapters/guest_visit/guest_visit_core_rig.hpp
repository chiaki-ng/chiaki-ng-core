#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/invitation_book.hpp"
#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/net/kakusan/link.hpp"
#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/play/source_stream.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/forwarded_media.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/source_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class GuestVisitCoreTest : public ::testing::Test {
 protected:
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using Progress = libchiaking::kakusan::Progress;
  using Clock = libchiaking::kakusan::Clock;
  using Error = libchiaking::net::kakusan::Error;
  using TransportRole = libchiaking::net::kakusan::TransportRole;
  using Lane = test_support::kakusan::Lane;

  static constexpr libchiaking::net::kakusan::SeatBinding seat{2, 5};
  static constexpr std::uint64_t start_us = 1'000'000;
  static constexpr std::uint64_t start_wall = 1'791'115'200;
  static constexpr std::string_view host_code_text = "4821";
  static constexpr std::uint16_t host_code_value = 4821;

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto media_key_position(const std::vector<std::byte>& datagram)
      -> std::optional<std::uint64_t> {
    if (datagram.size() < libchiaking::net::kakusan::media_header_size)
      return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t index = 44; index < 52; ++index)
      value = (value << 8U) | std::to_integer<std::uint64_t>(datagram[index]);
    return value;
  }

  class Rig {
   public:
    Rig() {
      auto created = GuestVisit::Core::create(
          GuestVisit::Core::Dependencies{platform, guest_entropy, factory,
                                         media},
          Identities::tagged(crypto(), 0x60));
      EXPECT_TRUE(created);
      guest = std::move(created).value();
      auto transport = factory.create(TransportRole::host);
      EXPECT_TRUE(transport);
      host_link = std::make_unique<libchiaking::net::kakusan::Link>(
          std::move(transport).value(), TransportRole::host);
      auto credentials =
          libchiaking::net::kakusan::ShortBlockCodec::credentials(
              crypto(), host_block_seed(),
              libchiaking::net::kakusan::BlockKind::invitation);
      EXPECT_TRUE(credentials);
      EXPECT_TRUE(host_link->gather(now, std::move(credentials).value()));
      step();
      EXPECT_TRUE(host_description);
      invitation.emplace(sign(start_wall));
      invitation_block = std::string{invitation->text.view()};
    }

    static auto host_block_seed() -> libchiaking::net::kakusan::BlockSeed {
      libchiaking::net::kakusan::BlockSeed seed{};
      seed.fill(std::byte{0x02});
      return seed;
    }

    auto hosting() const
        -> libchiaking::net::kakusan::control::Identity::Hosting {
      libchiaking::net::kakusan::control::Identity::Hosting value;
      value.session_id.fill(std::byte{0x01});
      value.local_seats = 1;
      return value;
    }

    auto wall() const -> std::uint64_t {
      return start_wall +
             (now - start_us) / libchiaking::microseconds_per_second;
    }

    auto clock() const -> Clock { return Clock{now, wall()}; }

    auto body(std::uint64_t created_at) const
        -> libchiaking::net::kakusan::ShortInvitation {
      return libchiaking::net::kakusan::ShortInvitation{
          .seed = host_block_seed(),
          .invite_expires_at =
              created_at +
              libchiaking::net::kakusan::invitation_lifetime_seconds,
          .source_protocol = setup.source_protocol,
          .capacity = 4,
          .dtls_sha256 = host_description->certificate_digest,
          .identity_hint = libchiaking::net::kakusan::ShortBlockCodec::hint_of(
              host_identity.key().bytes()),
          .ice = host_description->ice.clone(),
          .invitation_id = {}};
    }

    auto sign(std::uint64_t created_at) const
        -> libchiaking::net::kakusan::SealedInvitation {
      auto sealed = libchiaking::net::kakusan::ShortBlockCodec::seal(
          crypto(), body(created_at));
      EXPECT_TRUE(sealed);
      return std::move(sealed).value();
    }

    void accept() {
      const auto links_before = exchange->links.size();
      const auto preview = GuestVisit::Core::preview(crypto(), invitation_block,
                                                     clock().wall_seconds);
      ASSERT_TRUE(preview);
      EXPECT_EQ(preview.value().host_hint.bytes(),
                libchiaking::net::kakusan::ShortBlockCodec::hint_of(
                    host_identity.key().bytes()));
      EXPECT_EQ(exchange->links.size(), links_before);
      EXPECT_FALSE(GuestVisit::Core::preview(
          crypto(), invitation_block,
          start_wall + libchiaking::net::kakusan::invitation_lifetime_seconds));
      EXPECT_FALSE(GuestVisit::Core::preview(crypto(), "RCP1:AAAA",
                                             clock().wall_seconds));
      const auto summary = guest->accept_invitation(invitation_block, clock());
      ASSERT_TRUE(summary);
      EXPECT_EQ(summary.value().host_hint, preview.value().host_hint);
      EXPECT_EQ(summary.value().seats.capacity, 4U);
      EXPECT_EQ(
          summary.value().expires_at,
          start_wall + libchiaking::net::kakusan::invitation_lifetime_seconds);
      EXPECT_EQ(summary.value().stream_protocol, 12U);
    }

    void import_reply(const std::string& block) {
      auto parsed = libchiaking::net::kakusan::ShortBlockCodec::parse_reply(
          crypto(), block);
      ASSERT_TRUE(parsed);
      EXPECT_TRUE(libchiaking::net::kakusan::ShortBlockCodec::answers(
          *invitation, parsed.value()));
      EXPECT_EQ(parsed.value().body.identity_hint,
                libchiaking::net::kakusan::ShortBlockCodec::hint_of(
                    guest_public.key().bytes()));
      reply = std::move(parsed).value();
      if (!auto_import) return;
      const auto transcript = libchiaking::net::kakusan::Exchange::transcript(
          crypto(), invitation->hash, reply->hash);
      ASSERT_TRUE(transcript);
      libchiaking::kakusan::PairingContext context;
      context.transcript = transcript.value();
      context.parties.host_identity = host_identity.key().bytes();
      context.invitation_id = invitation->body.invitation_id;
      context.peer_hint = reply->body.identity_hint;
      host_context = context;
      apply(host.handle(libchiaking::kakusan::ReplyImported{now, context}));
      apply(host.handle(libchiaking::kakusan::AdmissionApproved{now}));
      const libchiaking::net::kakusan::TransportDescription remote{
          reply->body.ice.clone(), reply->body.dtls_sha256};
      EXPECT_TRUE(host_link->connect(remote, context.transcript, now));
    }

    void apply(libchiaking::kakusan::InvitedPlayer::Effects effects) {
      using namespace libchiaking::kakusan;
      for (auto& effect : effects) {
        if (const auto* send = std::get_if<SendControl>(&effect)) {
          static_cast<void>(host_link->send_control(send->message, send->token,
                                                    send->terminal, now));
        } else if (std::holds_alternative<ReserveSeat>(effect)) {
          apply(host.handle(SeatReserved{now, seat}));
        } else if (std::holds_alternative<RequestSourceJoin>(effect)) {
          apply(host.handle(
              SourceJoined{now, setup.clone(), 4, 1,
                           libchiaking::net::kakusan::button_mask}));
        } else if (const auto* input = std::get_if<ApplyInput>(&effect)) {
          applied.push_back(input->state);
          applied_at.push_back(now);
        } else if (const auto* feedback = std::get_if<SendFeedback>(&effect)) {
          static_cast<void>(host_link->send_feedback(feedback->frame));
        } else if (std::holds_alternative<StartMedia>(effect)) {
          ++host_media_started;
          auto key = host_link->media_key();
          ASSERT_TRUE(key);
          const auto connection =
              libchiaking::net::kakusan::TransportFrameCodec::connection_id(
                  crypto(), host_context->transcript);
          ASSERT_TRUE(connection);
          sender = std::make_unique<libchiaking::net::kakusan::MediaSender>(
              crypto(), std::move(key).value(), connection.value(), now);
        } else if (std::holds_alternative<StopMedia>(effect)) {
          sender.reset();
        } else if (std::holds_alternative<RequestIdr>(effect)) {
          ++idr_requests;
        } else if (std::holds_alternative<CloseTransport>(effect)) {
          ++host_closed;
          host_link->close();
        } else if (const auto* shown = std::get_if<ShowState>(&effect)) {
          if (shown->error) host_shown = shown->error;
        }
      }
    }

    void deliver_host(std::span<libchiaking::net::kakusan::LinkEvent> events) {
      using namespace libchiaking::kakusan;
      namespace link_event = libchiaking::net::kakusan::link_event;
      for (auto& event : events) {
        if (auto* gathered = std::get_if<link_event::Gathered>(&event))
          host_description = std::move(gathered->local);
        else if (std::holds_alternative<link_event::Nominated>(event))
          apply(host.handle(IceNominated{now}));
        else if (const auto* established =
                     std::get_if<link_event::Established>(&event))
          apply(host.handle(DtlsEstablished{
              now, established->exporter.clone(),
              libchiaking::net::kakusan::IdentityProof::sign(
                  crypto(), libchiaking::net::kakusan::Role::host,
                  host_context->transcript, established->exporter,
                  host_identity.saved().seed, hosting())
                  .value()}));
        else if (auto* message = std::get_if<link_event::Control>(&event)) {
          host_received.push_back(
              test_support::kakusan::Messages::clone(message->message));
          apply(host.handle(ControlReceived{now, std::move(message->message)}));
        } else if (const auto* acked = std::get_if<link_event::Acked>(&event))
          apply(host.handle(ControlAcked{now, acked->token}));
        else if (const auto* input = std::get_if<link_event::Input>(&event))
          apply(host.handle(InputReceived{now, input->frame}));
        else if (auto* ack = std::get_if<link_event::MediaAck>(&event)) {
          ++acks;
          if (sender) {
            EXPECT_TRUE(sender->on_ack(ack->record, now));
          }
        } else if (const auto* malformed =
                       std::get_if<link_event::Malformed>(&event))
          apply(host.handle(MalformedData{now, malformed->error}));
        else if (std::holds_alternative<link_event::Failed>(event))
          apply(host.handle(TransportFailed{now}));
      }
    }

    void collect() {
      while (auto output = guest->take_output()) {
        if (const auto* ready =
                std::get_if<GuestVisit::Core::ReplyReady>(&*output))
          import_reply(std::string{ready->block.view()});
        if (const auto* code =
                std::get_if<GuestVisit::Core::CodeReady>(&*output))
          guest_code = code->code.clone();
        if (std::holds_alternative<GuestVisit::Core::ControllerRequested>(
                *output) &&
            auto_ready) {
          EXPECT_TRUE(guest->controller_ready(now));
        }
        outputs.push_back(std::move(*output));
      }
    }

    void step(std::uint64_t advance_us = 1'000) {
      now += advance_us;
      auto events = host_link->pump(now);
      deliver_host(events);
      apply(host.on_time(now));
      if (sender) {
        std::vector<std::vector<std::byte>> datagrams;
        EXPECT_TRUE(sender->poll(now, datagrams));
        for (const auto& datagram : datagrams)
          static_cast<void>(host_link->send_media(datagram));
      }
      guest->pump(clock());
      collect();
    }

    void run(std::size_t steps, std::uint64_t advance_us = 1'000) {
      for (std::size_t index = 0; index < steps; ++index) step(advance_us);
    }

    void run_until(const std::function<bool()>& done,
                   std::size_t limit = 5'000) {
      for (std::size_t index = 0; index < limit && !done(); ++index) step();
      EXPECT_TRUE(done());
    }

    void connect() {
      accept();
      run_until([&] {
        return host.state() == Progress::verifying &&
               guest->state() == Progress::verifying && guest_code.has_value();
      });
    }

    void pair(std::string_view entered = host_code_text) {
      apply(host.handle(libchiaking::kakusan::LocalCode{
          now, libchiaking::net::kakusan::Code::from_value(host_code_value)
                   .value()}));
      ASSERT_TRUE(guest_code);
      apply(host.handle(
          libchiaking::kakusan::PeerCodeEntered{now, guest_code->clone()}));
      EXPECT_TRUE(guest->enter_code(entered, now));
    }

    void activate() {
      connect();
      pair();
      run_until([&] {
        return host.state() == Progress::playing &&
               guest->state() == Progress::playing && sender != nullptr;
      });
    }

    void forward(std::size_t index) {
      ASSERT_TRUE(sender);
      EXPECT_TRUE(sender->submit(SourceVectors::media_kind(index),
                                 SourceStream::vectors[index].key_position,
                                 SourceStream::datagram(index), now));
    }

    void forward_raw(libchiaking::net::kakusan::MediaKind kind,
                     std::uint64_t key_position,
                     const std::vector<std::byte>& datagram) {
      ASSERT_TRUE(sender);
      EXPECT_TRUE(sender->submit(kind, key_position, datagram, now));
    }

    auto wire_link() -> test_support::kakusan::LoopbackLink& {
      return *exchange->links.front();
    }

    template <class Output>
    auto all() const -> std::vector<Output> {
      std::vector<Output> found;
      for (const auto& output : outputs) {
        if (const auto* item = std::get_if<Output>(&output)) {
          if constexpr (std::is_same_v<Output, GuestVisit::Core::CodeReady>)
            found.push_back({item->code.clone()});
          else
            found.push_back(*item);
        }
      }
      return found;
    }

    template <class Output>
    auto count() const -> std::size_t {
      return static_cast<std::size_t>(
          std::ranges::count_if(outputs, [](const auto& output) {
            return std::holds_alternative<Output>(output);
          }));
    }

    auto last_state() const -> GuestVisit::Core::State {
      const auto states = all<GuestVisit::Core::State>();
      EXPECT_FALSE(states.empty());
      return states.empty() ? GuestVisit::Core::State{} : states.back();
    }

    auto rumbles() const -> std::vector<GuestVisit::Core::Rumble> {
      return all<GuestVisit::Core::Rumble>();
    }

    auto host_got_failure(Error code) const -> bool {
      return std::ranges::any_of(host_received, [code](const auto& item) {
        const auto* failure =
            std::get_if<libchiaking::net::kakusan::control::Failure>(&item);
        return failure != nullptr && failure->code == code;
      });
    }

    auto host_got_idr() const -> bool {
      return std::ranges::any_of(host_received, [](const auto& item) {
        return std::holds_alternative<
            libchiaking::net::kakusan::control::SourceIdr>(item);
      });
    }

    std::shared_ptr<test_support::kakusan::LoopbackExchange> exchange{
        std::make_shared<test_support::kakusan::LoopbackExchange>(crypto())};
    test_support::kakusan::LoopbackFactory factory{exchange};
    test_support::ScriptedRandom host_entropy{0x10};
    test_support::ScriptedRandom guest_entropy{0x90};
    libchiaking::kakusan::DeviceIdentity host_identity{
        Identities::tagged(crypto(), 0x40)};
    libchiaking::kakusan::DeviceIdentity guest_public{
        Identities::tagged(crypto(), 0x60)};
    std::unique_ptr<libchiaking::net::kakusan::Link> host_link;
    libchiaking::kakusan::InvitedPlayer host{crypto(), host_entropy};
    std::uint64_t now{start_us};
    std::optional<libchiaking::net::kakusan::TransportDescription>
        host_description;
    std::optional<libchiaking::net::kakusan::SealedInvitation> invitation;
    std::string invitation_block;
    std::optional<libchiaking::net::kakusan::SealedReply> reply;
    std::optional<libchiaking::kakusan::PairingContext> host_context;
    libchiaking::net::kakusan::control::SourceSetup setup{
        SourceVectors::setup()};
    std::unique_ptr<libchiaking::net::kakusan::MediaSender> sender;
    bool auto_import{true};
    bool auto_ready{true};

    std::vector<GuestVisit::Core::Output> outputs;
    ForwardedMedia media;
    TrackedClock stream_clock{now};
    libchiaking::platform::DefaultPlatformFactory platform{
        libchiaking::platform::PlatformProviders{.clock = &stream_clock,
                                                 .crypto = &crypto()}};
    std::unique_ptr<GuestVisit::Core> guest;
    std::optional<libchiaking::net::kakusan::Code> guest_code;
    std::vector<libchiaking::net::kakusan::InputState> applied;
    std::vector<std::uint64_t> applied_at;
    std::vector<libchiaking::net::kakusan::control::Message> host_received;
    std::size_t idr_requests{};
    std::size_t acks{};
    std::size_t host_media_started{};
    std::size_t host_closed{};
    std::optional<Error> host_shown;
  };

  static void expect_source_media(const ForwardedMedia& shown) {
    ASSERT_EQ(shown.headers.size(), 1U);
    EXPECT_EQ(shown.headers.front(),
              test_support::Bytes::unhex(SourceStream::video_header));
    ASSERT_EQ(shown.audio.size(), SourceStream::audio_frames.size());
    ASSERT_EQ(shown.video.size(), SourceStream::video_frames.size());
    for (std::size_t index = 0; index < shown.audio.size(); ++index)
      EXPECT_EQ(shown.audio[index],
                test_support::Bytes::unhex(SourceStream::audio_frames[index]));
    for (std::size_t index = 0; index < shown.video.size(); ++index)
      EXPECT_EQ(shown.video[index],
                test_support::Bytes::unhex(SourceStream::video_frames[index]));
  }

  static void expect_silent_rumble(const Rig& rig) {
    const auto latest = rig.rumbles();
    ASSERT_FALSE(latest.empty());
    EXPECT_EQ(latest.back().low_frequency, 0U);
    EXPECT_EQ(latest.back().high_frequency, 0U);
  }

  static void expect_stopped_by_peer(
      Rig& rig, libchiaking::net::kakusan::control::Reason reason) {
    EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 1U);
    expect_silent_rumble(rig);
    const auto states = rig.all<GuestVisit::Core::State>();
    const auto closing = std::ranges::find_if(states, [](const auto& item) {
      return item.state == Progress::leaving;
    });
    ASSERT_NE(closing, states.end());
    EXPECT_EQ(closing->reason, reason);
    EXPECT_TRUE(closing->by_peer);
    EXPECT_FALSE(closing->error);
    EXPECT_EQ(closing->seat, seat);
    rig.run(1100);
    EXPECT_EQ(rig.guest->state(), Progress::gone);
    EXPECT_EQ(rig.last_state().reason, reason);
    EXPECT_TRUE(rig.last_state().by_peer);
    EXPECT_FALSE(rig.guest->next_deadline_us());
    EXPECT_TRUE(rig.wire_link().sides[1].closed);
    EXPECT_EQ(rig.guest->forwarded(), nullptr);
  }
};

}  // namespace kakusan_test
