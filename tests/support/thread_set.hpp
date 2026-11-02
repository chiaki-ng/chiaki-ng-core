#pragma once

#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace test_support {

class ThreadSet {
 public:
  using Identity = libchiaking::platform::ThreadIdentity;

  void insert(Identity thread) {
    if (!contains(thread)) threads_.push_back(thread);
  }

  auto contains(Identity thread) const -> bool {
    return std::ranges::find(threads_, thread) != threads_.end();
  }

  auto size() const noexcept -> std::size_t { return threads_.size(); }
  auto empty() const noexcept -> bool { return threads_.empty(); }
  auto begin() const noexcept { return threads_.begin(); }
  auto end() const noexcept { return threads_.end(); }

  friend auto operator==(const ThreadSet& left, const ThreadSet& right)
      -> bool {
    return left.size() == right.size() &&
           std::ranges::all_of(left.threads_, [&right](Identity thread) {
             return right.contains(thread);
           });
  }

 private:
  std::vector<Identity> threads_;
};

}  // namespace test_support
