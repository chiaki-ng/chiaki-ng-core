#include "libchiaking/testing/kakusan/kakusan_pair.hpp"

#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/testing/fake_secure_datagram.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "libchiaking/testing/split_mix.hpp"
#include "src/core/kakusan/kakusan_core.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace libchiaking::kakusan::testing {

class KakusanPair::Core {
 public:
  class Random final : public platform::SecureRandomProvider {
   public:
    explicit Random(std::uint64_t seed) noexcept : sequence_(seed) {}

    [[nodiscard]] auto fill_bytes(std::span<std::byte> output)
        -> Expected<void, platform::PlatformError> override {
      sequence_.fill(output);
      return {};
    }

   private:
    libchiaking::testing::SplitMix64 sequence_;
  };

  Core(platform::PlatformFactory& platform, Time time)
      : platform_(platform),
        time_(time),
        network_(platform.threading(), platform.clock()) {}
  Core(const Core&) = delete;
  auto operator=(const Core&) -> Core& = delete;

  auto now() const noexcept -> Clock {
    if (time_ == Time::real)
      return Clock{
          platform_.clock().now_microseconds(),
          platform_.wall_clock().now_microseconds() / microseconds_per_second};
    const auto elapsed = elapsed_us_.load();
    return Clock{first_us + elapsed,
                 first_wall_second + elapsed / microseconds_per_second};
  }

  void advance(std::chrono::milliseconds duration) noexcept {
    if (duration.count() > 0)
      elapsed_us_.fetch_add(static_cast<std::uint64_t>(
          std::chrono::microseconds{duration}.count()));
  }

  [[nodiscard]] auto open(Devices devices, platform::DiagnosticsProvider* log)
      -> Expected<void, Kakusan::Unavailable> {
    if (!platform_.supplies(platform::PlatformCapability::crypto))
      return unexpected(Kakusan::Unavailable::no_identity_crypto);
    auto& crypto = platform_.crypto();
    const auto surround = [&](Random& random, DeviceStore& store) {
      return Surroundings::of(Surroundings::Injected{
          crypto, random, [this] { return now(); }, store,
          platform_.threading(), log, nullptr, &network_, &secure_datagram_});
    };
    auto hosting = surround(host_random_, devices.host);
    if (!hosting) return unexpected(hosting.error());
    auto visiting = surround(guest_random_, devices.guest);
    if (!visiting) return unexpected(visiting.error());
    host_.emplace(libchiaking::testing::ServiceBuilder::on<Kakusan>(
        std::move(hosting).value()));
    guest_.emplace(libchiaking::testing::ServiceBuilder::on<Kakusan>(
        std::move(visiting).value()));
    return {};
  }

  auto host() noexcept -> Kakusan& { return *host_; }
  auto guest() noexcept -> Kakusan& { return *guest_; }
  auto network() noexcept -> libchiaking::testing::InMemoryNetwork& {
    return network_;
  }

 private:
  static constexpr std::uint64_t first_us = 5'000'000;
  static constexpr std::uint64_t microseconds_per_second = 1'000'000;

  platform::PlatformFactory& platform_;
  Time time_;
  std::atomic<std::uint64_t> elapsed_us_{};
  libchiaking::testing::InMemoryNetwork network_;
  libchiaking::testing::FakeSecureDatagramProvider secure_datagram_{
      0x7368617265ULL};
  Random host_random_{0x686f7374ULL};
  Random guest_random_{0x6775657374ULL};
  std::optional<Kakusan> host_;
  std::optional<Kakusan> guest_;
};

KakusanPair::KakusanPair(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

KakusanPair::~KakusanPair() = default;

auto KakusanPair::create(platform::PlatformFactory& platform, Devices devices,
                         Time time, platform::DiagnosticsProvider* log)
    -> Expected<std::unique_ptr<KakusanPair>, Kakusan::Unavailable> {
  auto core = std::make_unique<Core>(platform, time);
  if (const auto opened = core->open(devices, log); !opened)
    return unexpected(opened.error());
  return std::make_unique<KakusanPair>(std::move(core));
}

auto KakusanPair::copied(const Invitation& invitation) -> std::string {
  return std::string{invitation.text()};
}

auto KakusanPair::copied(const Reply& reply) -> std::string {
  return std::string{reply.text()};
}

auto KakusanPair::host() noexcept -> Kakusan& { return core_->host(); }

auto KakusanPair::guest() noexcept -> Kakusan& { return core_->guest(); }

auto KakusanPair::network() noexcept -> libchiaking::testing::InMemoryNetwork& {
  return core_->network();
}

auto KakusanPair::now() const noexcept -> std::chrono::microseconds {
  return std::chrono::microseconds{
      static_cast<std::chrono::microseconds::rep>(core_->now().now_us)};
}

auto KakusanPair::wall() const noexcept
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point{
      std::chrono::seconds{core_->now().wall_seconds}};
}

void KakusanPair::advance(std::chrono::milliseconds duration) noexcept {
  core_->advance(duration);
}

}  // namespace libchiaking::kakusan::testing
