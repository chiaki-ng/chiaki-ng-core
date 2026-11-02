#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "libchiaking/platform/dtls/openssl/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/play_types.hpp"
#include "tests/unit/core/kakusan/manual_pair.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"
#include "tests/unit/core/kakusan/source_vectors.hpp"
#include "tests/support/play/source_stream.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using test_support::play::SourceStream;

namespace {

using namespace libchiaking;

class Demo final : public manual::HostEventSink {
 public:
  Demo(std::string invitation_path, std::string host_code_path)
      : invitation_path_(std::move(invitation_path)),
        host_code_path_(std::move(host_code_path)) {}

  void attach(manual::Host& host) noexcept { host_ = &host; }

  void on_event(manual::HostEvent event) override {
    if (const auto* invitation =
            std::get_if<manual::host_event::InvitationReady>(&event)) {
      std::ofstream file{invitation_path_, std::ios::binary | std::ios::trunc};
      const auto block = invitation->block.view();
      file.write(block.data(), static_cast<std::streamsize>(block.size()));
      invited_ = true;
    } else if (const auto* code =
                   std::get_if<manual::host_event::CodeReady>(&event)) {
      static_cast<void>(host_->approve(code->guest));
      guest_ = code->guest;
      verifying_ = true;
      if (!host_code_path_.empty()) {
        std::ofstream file{host_code_path_, std::ios::binary | std::ios::trunc};
        const auto digits = code->code.digits();
        file.write(digits.data(), static_cast<std::streamsize>(digits.size()));
      }
    } else if (const auto* join =
                   std::get_if<manual::host_event::JoinRequested>(&event)) {
      static_cast<void>(host_->source_joined(
          join->seat, libchiaking::controllers::SeatNumber::create(1).value()));
    } else if (const auto* changed =
                   std::get_if<manual::host_event::StateChanged>(&event)) {
      const auto state = static_cast<int>(changed->state);
      std::fprintf(stderr, "guest state %d error %d reason %d\n", state,
                   changed->error ? static_cast<int>(*changed->error) : 0,
                   changed->reason ? static_cast<int>(*changed->reason) : 0);
      if (changed->state != manual::Progress::leaving &&
          changed->state != manual::Progress::gone && state > furthest_)
        furthest_ = state;
      closed_ = closed_ || changed->state == manual::Progress::gone;
      active_ = active_ || changed->state == manual::Progress::playing;
    }
  }

  auto invited() const noexcept -> bool { return invited_; }
  auto verifying() const noexcept -> bool { return verifying_; }
  auto active() const noexcept -> bool { return active_; }
  auto closed() const noexcept -> bool { return closed_; }
  auto furthest() const noexcept -> int { return furthest_; }
  auto guest() const noexcept -> const manual::GuestId& { return guest_; }

  static auto read_file(const std::string& path) -> std::string {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
  }

 private:
  manual::Host* host_{};
  std::string invitation_path_;
  std::string host_code_path_;
  manual::GuestId guest_{};
  bool invited_{};
  bool verifying_{};
  bool active_{};
  int furthest_{-1};
  bool closed_{};
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 && argc != 5) return 2;
  const std::string reply_path = argv[2];
  const bool paired = argc == 5;
  const std::string guest_code_path = paired ? argv[4] : "";
  Demo demo{argv[1], paired ? argv[3] : ""};
  platform::openssl::OpenSslCryptoProvider crypto;
  platform::openssl::OpenSslSecureDatagramProvider secure_datagram;
  platform::SystemSecureRandom random;
  const auto identity = manual::DeviceIdentity::create(crypto, random);
  if (!identity) return 3;
  const auto rules = manual::SharingRules::create(4, 1, manual::all_buttons);
  auto environment = manual::Environment::system(crypto, secure_datagram);
  if (!rules || !environment) return 4;
  auto created = manual::Host::create(*rules, identity.value(),
                                      std::move(environment).value(), demo);
  if (!created) return 4;
  auto& host = created.value();
  demo.attach(host);

  host.source_keys(SourceStream::takion_version,
                   SourceStream::receive_keys());
  if (!host.source_started(SourceStream::description(),
                           SourceStream::video_fps))
    return 5;
  if (!host.invite()) return 6;

  platform::DefaultThreadingProvider threading;
  const platform::SteadyClock steady;
  platform::Sleeper sleeper{threading, steady};
  constexpr std::uint64_t run_us = 240'000'000;
  constexpr std::uint64_t forward_delay_us = 300'000;
  const auto deadline = steady.now_microseconds() + run_us;
  bool imported = false;
  bool entered = false;
  std::size_t forwarded = 0;
  std::optional<std::uint64_t> forward_at;
  while (steady.now_microseconds() < deadline && !demo.closed()) {
    static_cast<void>(host.pump());
    if (demo.invited() && !imported) {
      const auto reply = Demo::read_file(reply_path);
      if (reply.size() > 16) {
        const auto result = host.import_reply(reply);
        std::fprintf(
            stderr, "import %s\n",
            result ? "OK"
                   : std::string{manual::error_name(result.error())}.c_str());
        imported = result.has_value();
        if (!imported) break;
      }
    }
    if (paired && demo.verifying() && !entered) {
      const auto code = Demo::read_file(guest_code_path);
      if (code.size() >= manual::VerificationCode::size) {
        const auto result = host.enter_code(
            demo.guest(),
            std::string_view{code}.substr(0, manual::VerificationCode::size));
        std::fprintf(
            stderr, "enter code %s\n",
            result ? "OK"
                   : std::string{manual::error_name(result.error())}.c_str());
        entered = true;
      }
    }
    if (demo.active() && !forward_at)
      forward_at = steady.now_microseconds() + forward_delay_us;
    if (forward_at && steady.now_microseconds() >= *forward_at &&
        forwarded < SourceStream::vectors.size()) {
      const auto bytes = SourceStream::datagram(forwarded);
      host.source_datagram(kakusan_test::SourceDescriptions::describe(
          SourceStream::takion_version,
          SourceStream::vectors[forwarded].media,
          SourceStream::vectors[forwarded].key_position, bytes));
      ++forwarded;
    }
    sleeper.sleep_for(
        host.next_wait(std::chrono::milliseconds{imported ? 2 : 50}));
  }
  std::fprintf(stderr, "furthest state %d\n", demo.furthest());
  const auto wanted = static_cast<int>(paired ? manual::Progress::playing
                                              : manual::Progress::verifying);
  return demo.furthest() >= wanted ? 0 : 1;
}
