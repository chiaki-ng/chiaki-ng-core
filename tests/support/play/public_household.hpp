#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/setup.hpp"

#include <libchiaking/consoles.hpp>
#include <libchiaking/platform/factory.hpp>
#include <libchiaking/play.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/play/scripted_link.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace test_support::play {

using test_support::LiveProviders;

class PublicDatacenters final : public libchiaking::play::DatacenterStore {
 public:
  using Service = libchiaking::catalog::ServiceGeneration;

  auto load(Service service) -> libchiaking::Expected<
      std::vector<libchiaking::play::DatacenterMeasurement>, Error> override {
    return held_[service];
  }

  auto save(
      Service service,
      std::span<const libchiaking::play::DatacenterMeasurement> measurements)
      -> libchiaking::Expected<void, Error> override {
    held_[service].assign(measurements.begin(), measurements.end());
    return {};
  }

 private:
  std::map<Service, std::vector<libchiaking::play::DatacenterMeasurement>>
      held_;
};

class PublicEvents final : public libchiaking::play::SessionEvents {
 public:
  void on_state(libchiaking::play::SessionState state) override {
    states.push_back(state);
  }
  void on_ended(const libchiaking::play::Outcome& outcome) override {
    left = outcome.was_left();
  }
  void on_launch_progress(
      const libchiaking::play::LaunchProgress& progress) override {
    launches.push_back(progress);
  }
  void on_seat_joined(libchiaking::controllers::SeatNumber seat,
                      libchiaking::controllers::SeatLight) override {
    joined.push_back(seat);
  }
  void on_seat_left(libchiaking::controllers::SeatNumber seat) override {
    departed.push_back(seat);
  }

  auto reached(libchiaking::play::SessionState state) const -> bool {
    return std::ranges::find(states, state) != states.end();
  }

  std::vector<libchiaking::play::SessionState> states;
  std::vector<libchiaking::play::LaunchProgress> launches;
  std::vector<libchiaking::controllers::SeatNumber> joined;
  std::vector<libchiaking::controllers::SeatNumber> departed;
  std::optional<bool> left;
};

class PublicMedia final : public libchiaking::media::MediaSink {
 public:
  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    descriptions.push_back(description);
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    const auto bytes = unit.frame().bytes();
    auto& kept = unit.frame().profile_header() ? headers : video;
    kept.emplace_back(bytes.begin(), bytes.end());
    frame_indexes.push_back(unit.frame().frame_index());
    return {};
  }

  void on_video_gap() override { ++gaps; }

  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    if (unit.kind == libchiaking::media::AudioUnit::Kind::samples)
      audio.push_back(unit.bytes);
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback& feedback) override {
    feedback_seen.push_back(feedback);
  }

  std::vector<libchiaking::media::StreamDescription> descriptions;
  std::vector<std::vector<std::byte>> video;
  std::vector<std::vector<std::byte>> headers;
  std::vector<std::uint16_t> frame_indexes;
  std::vector<std::vector<std::byte>> audio;
  std::vector<libchiaking::controllers::ControllerFeedback> feedback_seen;
  std::size_t gaps{};
};

class PublicHousehold {
 public:
  PublicHousehold() : PublicHousehold(nullptr) {}

  explicit PublicHousehold(libchiaking::platform::ThreadingProvider* threading)
      : factory(libchiaking::platform::PlatformProviders{.threading = threading,
                                                         .network = &network,
                                                         .crypto = &crypto,
                                                         .http = &offline}),
        pairer(factory),
        known(libchiaking::testing::ServiceBuilder::build<
              libchiaking::consoles::KnownConsoles>(
            network, offline, crypto, random, store, LiveProviders::threading(),
            LiveProviders::steady(), &pairer)),
        play(libchiaking::testing::ServiceBuilder::build<
             libchiaking::play::Play>(factory, known,
                                      catalog::IdleCatalog::shared(),
                                      datacenters, pairer)) {}

  PublicHousehold(const PublicHousehold&) = delete;
  auto operator=(const PublicHousehold&) -> PublicHousehold& = delete;

  auto home_user(std::uint64_t account = 40)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return Setup::value(
        household.users.add_home_user(libchiaking::AccountId{account},
                                      "Living Room Player"),
        "the home user is added");
  }

  SoftwareCrypto crypto;
  libchiaking::testing::InMemoryNetwork network{LiveProviders::threading(),
                                                LiveProviders::steady()};
  users::Offline offline;
  users::PatternedRandom random;
  libchiaking::platform::DefaultPlatformFactory factory;
  libchiaking::testing::InMemoryConsoleStore store{LiveProviders::threading()};
  PublicDatacenters datacenters;
  users::SignedIn household;
  libchiaking::play::InternetPairer pairer;
  libchiaking::consoles::KnownConsoles known;
  libchiaking::play::Play play;
};

}  // namespace test_support::play
