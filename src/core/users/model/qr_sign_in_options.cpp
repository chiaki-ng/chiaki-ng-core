#include "libchiaking/detail/users/qr_sign_in.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/utf8.hpp"

#include <algorithm>

namespace libchiaking::users {

auto QrSignInOptions::device_name_usable() const noexcept -> bool {
  const auto name = device_name();
  return !name.empty() && name.size() <= maximum_device_name_bytes &&
         text::utf8::valid(name) &&
         std::ranges::none_of(name, text::ascii_control);
}

}  // namespace libchiaking::users
