#include "src/core/psn/graphql/title_media.hpp"

namespace libchiaking::psn::graphql {

MediaList::MediaList(Listed listed) {
  if (!listed) return;
  for (auto& entry : *listed) {
    if (!entry) continue;
    StoreMedia item{std::move(entry->role).value_or(std::string{}),
                    std::move(entry->type).value_or(std::string{}),
                    std::move(entry->url).value_or(std::string{})};
    if (!item.role.empty() && !item.url.empty())
      entries_.push_back(std::move(item));
  }
}

auto MediaList::title_media() const -> TitleMedia {
  TitleMedia result;
  std::string master;
  std::string edition_art;
  std::string background;
  std::string portrait;
  for (const auto& item : entries_) {
    const auto& role = item.role;
    const auto& type = item.type;
    const auto& url = item.url;
    if (role == "PREVIEW" && type == "VIDEO") {
      if (result.preview_video.empty()) result.preview_video = url;
      continue;
    }
    if (type != "IMAGE") continue;
    if (role == "LOGO" && result.logo.empty())
      result.logo = url;
    else if (role == "GAMEHUB_COVER_ART" && result.cover.empty())
      result.cover = url;
    else if (role == "PORTRAIT_BANNER" && portrait.empty())
      portrait = url;
    else if (role == "MASTER" && master.empty())
      master = url;
    else if (role == "EDITION_KEY_ART" && edition_art.empty())
      edition_art = url;
    else if (role == "BACKGROUND_LAYER_ART" && background.empty())
      background = url;
    else if (role == "SCREENSHOT" &&
             result.screenshots.size() < maximum_screenshots)
      result.screenshots.push_back(url);
  }
  if (result.cover.empty()) result.cover = std::move(portrait);
  result.key_art = !master.empty()        ? std::move(master)
                   : !edition_art.empty() ? std::move(edition_art)
                                          : std::move(background);
  return result;
}

}  // namespace libchiaking::psn::graphql
