#include "libchiaking/detail/kakusan/remembered_devices.hpp"

#include "libchiaking/detail/kakusan/device_identity.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <simdutf.h>

#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

namespace libchiaking::kakusan {

RememberedDevices::RememberedDevices(platform::CryptoProvider& crypto,
                                     platform::ThreadingProvider& threading,
                                     DeviceStore& store,
                                     std::vector<RememberedDevice> devices)
    : crypto_(crypto),
      store_(store),
      mutex_(threading),
      devices_(std::move(devices)) {}

auto RememberedDevices::load(platform::CryptoProvider& crypto,
                             platform::ThreadingProvider& threading,
                             DeviceStore& store)
    -> Expected<std::unique_ptr<RememberedDevices>, DeviceStore::Error> {
  auto saved = store.load_devices();
  if (!saved) return unexpected(saved.error());
  std::vector<RememberedDevice> devices;
  devices.reserve(saved.value().size());
  for (auto& device : saved.value()) {
    const auto print = DeviceIdentity::fingerprint_of(crypto, device.key);
    if (!print) return unexpected(DeviceStore::Error::unavailable);
    devices.push_back(
        RememberedDevice{device.key, print.value(), std::move(device.nickname),
                         device.first_verified, device.last_seen});
  }
  return std::unique_ptr<RememberedDevices>{
      new RememberedDevices{crypto, threading, store, std::move(devices)}};
}

auto RememberedDevices::folded(std::string_view nickname) -> std::u32string {
  static constexpr std::u32string_view latin_bases =
      U"aaaaaaæceeeeiiiiðnooooo÷øuuuuyþß";
  std::u32string points(
      simdutf::utf32_length_from_utf8(nickname.data(), nickname.size()), U'\0');
  const auto decoded = simdutf::convert_utf8_to_utf32(
      nickname.data(), nickname.size(), points.data());
  if (decoded == 0 && !nickname.empty()) {
    points.assign(nickname.begin(), nickname.end());
    return points;
  }
  points.resize(decoded);
  std::u32string folded;
  folded.reserve(points.size());
  for (auto point : points) {
    if (point >= U'A' && point <= U'Z')
      point += U'a' - U'A';
    else if (point >= 0xc0 && point <= 0xdf)
      point = latin_bases[point - 0xc0];
    else if (point >= 0xe0 && point <= 0xff && point != 0xf7)
      point = point == 0xff ? U'y' : latin_bases[point - 0xe0];
    if (point < 0x300 || point > 0x36f) folded.push_back(point);
  }
  return folded;
}

auto RememberedDevices::normalised(std::string_view nickname)
    -> std::optional<std::string> {
  const auto trimmed = text::trim(nickname);
  if (trimmed.empty()) return std::nullopt;
  for (std::size_t index = 0; index < trimmed.size(); ++index) {
    const auto unit = static_cast<unsigned char>(trimmed[index]);
    const bool c1 = unit == 0xc2 && index + 1 < trimmed.size() &&
                    static_cast<unsigned char>(trimmed[index + 1]) < 0xa0;
    if (unit < 0x20 || unit == 0x7f || c1) return std::nullopt;
  }
  if (simdutf::count_utf8(trimmed.data(), trimmed.size()) > longest_nickname)
    return std::nullopt;
  return std::string{trimmed};
}

auto RememberedDevices::same_nickname(std::string_view left,
                                      std::string_view right) -> bool {
  return folded(left) == folded(right);
}

auto RememberedDevices::saved(const RememberedDevice& device) -> SavedDevice {
  return SavedDevice{device.key, device.nickname, device.first_verified,
                     device.last_seen, device.fingerprint};
}

auto RememberedDevices::named(std::string_view nickname) const
    -> const RememberedDevice* {
  const auto found = std::ranges::find_if(
      devices_, [nickname](const RememberedDevice& device) {
        return same_nickname(device.nickname, nickname);
      });
  return found == devices_.end() ? nullptr : &*found;
}

auto RememberedDevices::held(const IdentityKey& key) -> RememberedDevice* {
  const auto found = std::ranges::find(devices_, key, &RememberedDevice::key);
  return found == devices_.end() ? nullptr : &*found;
}

auto RememberedDevices::all() const -> std::vector<RememberedDevice> {
  const platform::Lock lock{mutex_};
  return devices_;
}

auto RememberedDevices::recognise(
    const IdentityKey& key,
    std::optional<std::string_view> expected_nickname) const -> Recognition {
  const platform::Lock lock{mutex_};
  if (expected_nickname) {
    if (const auto* expected = named(*expected_nickname))
      return Recognition{expected->key == key ? Recognition::Kind::recognised
                                              : Recognition::Kind::changed_key,
                         *expected};
  }
  const auto known = std::ranges::find(devices_, key, &RememberedDevice::key);
  if (known != devices_.end())
    return Recognition{Recognition::Kind::recognised, *known};
  return {};
}

auto RememberedDevices::recognise(const IdentityHint& hint) const
    -> Recognition {
  const platform::Lock lock{mutex_};
  const auto known =
      std::ranges::find_if(devices_, [&](const RememberedDevice& device) {
        return std::ranges::equal(
            hint.bytes(),
            std::span{device.key.bytes()}.first(IdentityHint::size));
      });
  if (known != devices_.end())
    return Recognition{Recognition::Kind::recognised, *known};
  return {};
}

auto RememberedDevices::remember(const IdentityKey& key,
                                 std::string_view nickname, Replace replace,
                                 std::chrono::system_clock::time_point now)
    -> Expected<RememberedDevice, RememberRefusal> {
  const auto name = normalised(nickname);
  if (!name)
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::invalid_nickname, {}});
  const platform::Lock lock{mutex_};
  std::optional<IdentityKey> replaced;
  if (const auto* holder = named(*name);
      holder != nullptr && holder->key != key) {
    if (replace == Replace::no)
      return unexpected(
          RememberRefusal{RememberRefusal::Kind::nickname_taken, *holder});
    replaced = holder->key;
  }
  const auto* held_before = held(key);
  const auto before = held_before != nullptr
                          ? std::optional<RememberedDevice>{*held_before}
                          : std::nullopt;
  RememberedDevice device{
      key, {}, *name, before ? before->first_verified : now, now};
  if (before) {
    device.fingerprint = before->fingerprint;
  } else {
    const auto print = DeviceIdentity::fingerprint_of(crypto_, key);
    if (!print)
      return unexpected(
          RememberRefusal{RememberRefusal::Kind::store_unavailable, {}});
    device.fingerprint = print.value();
  }
  if (!store_.save(saved(device)))
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::store_unavailable, {}});
  if (replaced && !store_.remove(*replaced)) {
    std::ignore = before ? store_.save(saved(*before)) : store_.remove(key);
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::store_unavailable, {}});
  }
  if (replaced)
    std::erase_if(devices_, [&replaced](const RememberedDevice& remembered) {
      return remembered.key == *replaced;
    });
  if (auto* existing = held(key))
    *existing = device;
  else
    devices_.push_back(device);
  return device;
}

auto RememberedDevices::rename(const IdentityKey& key,
                               std::string_view nickname)
    -> Expected<RememberedDevice, RememberRefusal> {
  const auto name = normalised(nickname);
  if (!name)
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::invalid_nickname, {}});
  const platform::Lock lock{mutex_};
  if (const auto* holder = named(*name);
      holder != nullptr && holder->key != key)
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::nickname_taken, *holder});
  auto* known = held(key);
  if (known == nullptr)
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::unknown_device, {}});
  auto renamed = *known;
  renamed.nickname = *name;
  if (!store_.save(saved(renamed)))
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::store_unavailable, {}});
  *known = renamed;
  return renamed;
}

auto RememberedDevices::forget(const IdentityKey& key)
    -> Expected<void, DeviceStore::Error> {
  const platform::Lock lock{mutex_};
  if (const auto removed = store_.remove(key); !removed)
    return unexpected(removed.error());
  std::erase_if(devices_, [&key](const RememberedDevice& device) {
    return device.key == key;
  });
  return {};
}

void RememberedDevices::seen(const IdentityKey& key,
                             std::chrono::system_clock::time_point now) {
  const platform::Lock lock{mutex_};
  auto* known = held(key);
  if (known == nullptr) return;
  auto touched = *known;
  touched.last_seen = now;
  if (store_.save(saved(touched))) *known = touched;
}

}  // namespace libchiaking::kakusan
