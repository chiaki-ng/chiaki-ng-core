#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/outcome_checks.hpp"
#include "tests/unit/core/play/session_fakes.hpp"
#include "tests/support/play/session_rig.hpp"

#include <libchiaking/play_types.hpp>

#include <gtest/gtest.h>

#include <cstddef>

namespace play_test {

class SessionCase : public ::testing::Test {
 protected:
  static auto seat(std::size_t index) -> libchiaking::controllers::SeatNumber {
    return libchiaking::controllers::SeatNumber::create(index).value();
  }

  static auto description() -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription description;
    description.audio.channels = 2;
    description.audio.rate = 48000;
    description.profiles.push_back({1280, 720, api_test::bytes_of({1, 2, 3})});
    description.picture = libchiaking::media::GrantedPicture{
        libchiaking::media::VideoCodec::h265, 1280, 720, 60, 10000};
    return description;
  }
};

}  // namespace play_test
