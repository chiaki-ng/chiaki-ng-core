#include "current_thread.hpp"

#include <pthread.h>

#include <cstddef>
#include <string>

namespace libchiaking::platform::threading {

void CurrentThread::rename(std::string_view name) noexcept {
  constexpr std::size_t max_thread_name_length = 63;
  try {
    const std::string terminated{name.substr(0, max_thread_name_length)};
    (void)::pthread_setname_np(terminated.c_str());
  } catch (...) {
  }
}

}  // namespace libchiaking::platform::threading
