#pragma once

#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/testing/split_mix.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace test_support {

class FixedRandom final : public libchiaking::platform::SecureRandomProvider {
 public:
  FixedRandom() = default;
  explicit FixedRandom(std::byte fill) noexcept : fill_(fill) {}

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    std::ranges::fill(output, fill_);
    return {};
  }

  unsigned calls{};

 private:
  std::byte fill_{0x42};
};

class CountingRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    last_size = output.size();
    if (fail)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::system_failure);
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = static_cast<std::byte>(index);
    return {};
  }

  unsigned calls{};
  std::size_t last_size{};
  bool fail{};
};

class FailingRandom final : public libchiaking::platform::SecureRandomProvider {
 public:
  FailingRandom() = default;
  explicit FailingRandom(libchiaking::platform::PlatformError error) noexcept
      : error_(error) {}

  auto fill_bytes(std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    return libchiaking::unexpected(error_);
  }

  unsigned calls{};

 private:
  libchiaking::platform::PlatformError error_{
      libchiaking::platform::PlatformError::io_failure};
};

class SeededRandom final : public libchiaking::platform::SecureRandomProvider {
 public:
  explicit SeededRandom(std::uint64_t seed) noexcept : sequence_(seed) {}

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    sequence_.fill(output);
    return {};
  }

  unsigned calls{};

 private:
  libchiaking::testing::SplitMix64 sequence_;
};

class ScriptedRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  ScriptedRandom() = default;
  explicit ScriptedRandom(std::uint8_t start) noexcept : counter(start) {}

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++calls;
    if (next < queue.size() && queue[next].size() == output.size()) {
      std::ranges::copy(queue[next], output.begin());
      ++next;
      return {};
    }
    for (auto& byte : output) byte = std::byte{counter++};
    return {};
  }

  std::vector<std::vector<std::byte>> queue;
  std::size_t next{};
  std::uint8_t counter{};
  unsigned calls{};
};

}  // namespace test_support
