#pragma once

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <chrono>

namespace platform_net_test {

class SocketRig : public ::testing::Test {
 protected:
  void pause(std::chrono::nanoseconds duration) {
    libchiaking::platform::Sleeper sleeper{factory.threading(),
                                           factory.clock()};
    static_cast<void>(sleeper.sleep_for(duration));
  }

  auto sockets() -> libchiaking::platform::SocketProvider& {
    return factory.network().sockets();
  }

  libchiaking::platform::DefaultPlatformFactory factory;
};

}  // namespace platform_net_test
