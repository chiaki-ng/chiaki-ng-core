/// @file
/// @brief Declares KakusanPair, a Kakusan host and guest joined in one
/// process.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/device_store.hpp"
#include "libchiaking/detail/kakusan/invitation.hpp"
#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/testing/in_memory_network.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace libchiaking::kakusan::testing {

/// @brief Two Kakusan instances, a host and a guest, that can reach each
/// other without the internet.
///
/// The pair stands in for two machines. Both sides share one private
/// InMemoryNetwork and one FakeSecureDatagramProvider, each has its own fixed
/// random source and its own DeviceStore, and both read one clock. A test can
/// therefore take an invitation from host(), hand it to guest(), carry the
/// reply back and run a visit exactly as two frontends would, with repeatable
/// results.
///
/// The cryptography, threading and clocks come from the caller's platform
/// factory, so pass one with a working platform::CryptoProvider. What differs from two real devices is the
/// transport: no STUN, no real network, and no real DTLS.
///
/// Create the pair with create(), use copied() to move invitation and reply
/// text between the two sides as a user's clipboard would, and with
/// Time::by_hand call advance() to let timers run.
class KakusanPair {
 public:
  /// @brief Implementation state, defined inside the library.
  class Core;

  /// @brief Which clock both sides read.
  enum class Time : std::uint8_t {
    /// @brief Starts at a fixed instant and moves only when advance() is
    /// called, so every timeout is under the test's control.
    by_hand,
    /// @brief Follows the platform's monotonic and wall clocks; advance()
    /// then has no effect.
    real,
  };

  /// @brief The device stores the two sides keep their identities and known
  /// devices in.
  ///
  /// Both must outlive the pair. Use two separate stores, as two machines
  /// would.
  struct Devices {
    /// @brief The store for the hosting side.
    DeviceStore& host;
    /// @brief The store for the visiting side.
    DeviceStore& guest;
  };

  /// @brief The wall-clock time, in seconds since the Unix epoch, at which a
  /// `by_hand` pair starts.
  static constexpr std::uint64_t first_wall_second = 1'900'000'000;

  /// @brief Builds both sides and connects them.
  ///
  /// @param platform Supplies the cryptography, threading and clocks both
  ///     sides use. It must outlive the pair.
  /// @param devices The two sides' device stores.
  /// @param time Which clock to use; see Time.
  /// @param log Where both sides write diagnostics, or null for none. It must
  ///     outlive the pair.
  /// @return The pair, ready for the host to invite and the guest to join.
  ///     Fails with the Kakusan::Unavailable value that setting up either
  ///     side reported; Kakusan::Unavailable::no_identity_crypto when the
  ///     platform has no crypto provider.
  [[nodiscard]] static auto create(platform::PlatformFactory& platform,
                                   Devices devices, Time time = Time::by_hand,
                                   platform::DiagnosticsProvider* log = nullptr)
      -> Expected<std::unique_ptr<KakusanPair>, Kakusan::Unavailable>;

  /// @brief Returns an invitation as the text a host would copy and send.
  ///
  /// @param invitation The invitation to copy.
  /// @return An owned copy of the invitation's text.
  static auto copied(const Invitation& invitation) -> std::string;
  /// @brief Returns a reply as the text a guest would copy and send back.
  ///
  /// @param reply The reply to copy.
  /// @return An owned copy of the reply's text.
  static auto copied(const Reply& reply) -> std::string;

  /// @brief Wraps state that create() has already set up.
  ///
  /// Tests call create() instead.
  ///
  /// @param core The connected pair's state.
  explicit KakusanPair(std::unique_ptr<Core> core) noexcept;
  /// @brief Not copyable; the two sides refer to state the pair owns.
  KakusanPair(const KakusanPair&) = delete;
  /// @brief Not assignable; the two sides refer to state the pair owns.
  auto operator=(const KakusanPair&) -> KakusanPair& = delete;
  /// @brief Destroys both sides and their network.
  ///
  /// Anything obtained from host() or guest() must be released first.
  ~KakusanPair();

  /// @brief Returns the side that shares its session.
  /// @return The host's Kakusan, valid while the pair lives.
  auto host() noexcept -> Kakusan&;
  /// @brief Returns the side that visits.
  /// @return The guest's Kakusan, valid while the pair lives.
  auto guest() noexcept -> Kakusan&;
  /// @brief Returns the private network both sides use.
  ///
  /// A test can script faults on it, for example failing sends to play a
  /// connection that drops.
  ///
  /// @return The network, valid while the pair lives.
  auto network() noexcept -> libchiaking::testing::InMemoryNetwork&;
  /// @brief Returns the monotonic time both sides currently see.
  ///
  /// @return Microseconds on the pair's clock. A `by_hand` pair starts at
  ///     five seconds.
  auto now() const noexcept -> std::chrono::microseconds;
  /// @brief Returns the wall-clock time both sides currently see.
  ///
  /// @return The time in whole seconds. A `by_hand` pair starts at
  ///     first_wall_second and moves with advance().
  auto wall() const noexcept -> std::chrono::system_clock::time_point;
  /// @brief Moves a `by_hand` clock forward.
  ///
  /// Both the monotonic and the wall time move. Nothing runs as a result of
  /// the call itself; the two sides notice the new time when they next do
  /// work.
  ///
  /// @param duration How far to move. Zero and negative values are ignored.
  void advance(std::chrono::milliseconds duration) noexcept;

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::kakusan::testing
