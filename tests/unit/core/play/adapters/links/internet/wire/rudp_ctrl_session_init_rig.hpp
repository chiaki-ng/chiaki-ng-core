#pragma once

#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "tests/support/clock.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace play_test::rudp_ctrl {

class SessionInitRig : public ::testing::Test {
 protected:
  static constexpr rudp::HandshakeParameters handshake{0x1234, 0xA1B2C3D4U};
  static constexpr rudp::HandshakeParameters reference_handshake{0x4802,
                                                                 0x04058607U};

  SessionInitRig() { console_.target = remote_play::RegistrationTarget::ps5_1; }

  auto run(std::uint64_t attempt = 5,
           rudp::HandshakeParameters parameters = handshake) {
    return remote_play::RemoteSessionInit::run(
        channel_, parameters, clock_, {}, attempt, console_, "127.0.0.1", 9295);
  }

  auto console() -> remote_play::RegisteredConsole& { return console_; }

  test_support::FakeClock clock_{0};
  ExchangeChannel channel_{clock_};

 private:
  remote_play::RegisteredConsole console_;
};

}  // namespace play_test::rudp_ctrl
