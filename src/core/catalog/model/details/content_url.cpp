#include "libchiaking/detail/catalog/content_url.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::catalog {

template <class Host>
ContentUrl<Host>::ContentUrl(std::string url) noexcept : url_(std::move(url)) {}

template <class Host>
auto ContentUrl<Host>::at(std::string_view url) -> std::optional<ContentUrl> {
  const auto origin = origin_of(url);
  if (!origin || url.size() <= origin->size() || url.size() > url_limit)
    return std::nullopt;
  const auto path = url.substr(origin->size());
  if (!std::ranges::all_of(path, url_character) ||
      path.find("..") != std::string_view::npos)
    return std::nullopt;
  return ContentUrl{std::string{url}};
}

template <class Host>
auto ContentUrl<Host>::origin_of(std::string_view url) noexcept
    -> std::optional<std::string_view> {
  const auto found = std::ranges::find_if(
      origins,
      [url](std::string_view origin) { return url.starts_with(origin); });
  if (found == origins.end()) return std::nullopt;
  return *found;
}

template <class Host>
auto ContentUrl<Host>::url_character(char value) noexcept -> bool {
  constexpr std::string_view extra = "-._~/?=&%+";
  return text::ascii_alnum(value) ||
         extra.find(value) != std::string_view::npos;
}

template class ContentUrl<ImageHost>;
template class ContentUrl<TrailerHost>;

}  // namespace libchiaking::catalog
