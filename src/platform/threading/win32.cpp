#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "current_thread.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

namespace libchiaking::platform::threading {

void CurrentThread::rename(std::string_view name) noexcept {
  using SetThreadDescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const SetThreadDescriptionFunction setter = [] {
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return SetThreadDescriptionFunction{};
    return reinterpret_cast<SetThreadDescriptionFunction>(
        reinterpret_cast<void*>(
            ::GetProcAddress(kernel, "SetThreadDescription")));
  }();
  try {
    if (name.empty() || setter == nullptr) return;
    const auto bounded_size = std::min(
        name.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const int input_size = static_cast<int>(bounded_size);
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, name.data(), input_size, nullptr, 0);
    if (size <= 0) return;
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, name.data(), input_size, wide.data(),
                              size) != size)
      return;
    (void)setter(::GetCurrentThread(), wide.c_str());
  } catch (...) {
  }
}

}  // namespace libchiaking::platform::threading
