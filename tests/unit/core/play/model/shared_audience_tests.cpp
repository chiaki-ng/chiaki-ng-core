#include <libchiaking/play.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
namespace play = libchiaking::play;
using libchiaking::SecretArray;

class SharedAudience : public ::testing::Test {
 protected:
  class QuietAudience final : public play::SharedAudience {
   public:
    void stream_started(const play::SharedStream& stream,
                        play::ReceiveKeys keys) override {
      version_ = stream.protocol_version;
      first_key_byte_ = keys.key()[0];
    }
    void encrypted_datagram(
        const play::SharedDatagram& datagram) noexcept override {
      bytes_ += datagram.bytes.size();
    }
    void feedback(const controllers::ControllerFeedback&) override {}
    void guest_seated(controllers::SeatNumber seat) override { seated_ = seat; }
    void guest_seat_refused(const play::SeatRefusal&) override {}
    void guest_unseated(controllers::SeatNumber) override {}
    void became_unshareable(play::NotShareable) override {}
    auto service(std::chrono::milliseconds wait)
        -> std::chrono::milliseconds override {
      return wait;
    }
    void session_ended() override { ended_ = true; }

    auto version() const -> std::uint32_t { return version_; }
    auto first_key_byte() const -> std::byte { return first_key_byte_; }
    auto bytes() const -> std::size_t { return bytes_; }
    auto seated() const -> std::optional<controllers::SeatNumber> {
      return seated_;
    }
    auto ended() const -> bool { return ended_; }

   private:
    std::uint32_t version_{};
    std::byte first_key_byte_{};
    std::size_t bytes_{};
    std::optional<controllers::SeatNumber> seated_;
    bool ended_{};
  };

  static auto keys() -> play::ReceiveKeys {
    SecretArray<play::ReceiveKeys::size> key;
    SecretArray<play::ReceiveKeys::size> iv;
    key.fill(std::byte{0x5a});
    iv.fill(std::byte{0x11});
    return play::ReceiveKeys{std::move(key), std::move(iv)};
  }
};

TEST_F(SharedAudience, ReceiveKeysCloneTheirBytes) {
  const auto original = keys();
  const auto copy = original.clone();
  EXPECT_EQ(copy.key()[15], std::byte{0x5a});
  EXPECT_EQ(copy.iv()[0], std::byte{0x11});
}

TEST_F(SharedAudience, CanBeImplementedByTheApp) {
  QuietAudience quiet;
  play::SharedAudience& audience = quiet;
  audience.stream_started(
      {.protocol_version = 12, .where = play::Where::internet}, keys());
  const std::array<std::byte, 3> payload{};
  audience.encrypted_datagram(
      {.media = play::SharedDatagram::Media::audio, .bytes = payload});
  audience.guest_seated(*controllers::SeatNumber::create(1));
  audience.session_ended();
  EXPECT_EQ(quiet.version(), 12u);
  EXPECT_EQ(quiet.first_key_byte(), std::byte{0x5a});
  EXPECT_EQ(quiet.bytes(), 3u);
  EXPECT_EQ(quiet.seated(), *controllers::SeatNumber::create(1));
  EXPECT_TRUE(quiet.ended());
  EXPECT_EQ(audience.service(std::chrono::milliseconds{8}),
            std::chrono::milliseconds{8});
}

}  // namespace
}  // namespace play_test
