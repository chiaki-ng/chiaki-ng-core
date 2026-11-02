#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/random.hpp"
#include "tests/support/play/source_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class SourceVectors {
 public:
  static auto media_kind(std::size_t index)
      -> libchiaking::net::kakusan::MediaKind {
    return SourceStream::vectors[index].media ==
                   SourceStream::Media::audio
               ? libchiaking::net::kakusan::MediaKind::audio
               : libchiaking::net::kakusan::MediaKind::video;
  }

  static auto setup(const libchiaking::net::kakusan::control::SourceSessionId&
                        session_id = {})
      -> libchiaking::net::kakusan::control::SourceSetup {
    const auto keys = SourceStream::receive_keys();
    libchiaking::net::kakusan::control::SourceSetup made;
    made.source_session_id = session_id;
    made.source_protocol =
        static_cast<std::uint8_t>(SourceStream::takion_version);
    std::ranges::copy(keys.key(), made.receive_key.begin());
    std::ranges::copy(keys.iv(), made.receive_iv.begin());
    made.video_fps = SourceStream::video_fps;
    made.video_profiles.push_back(
        libchiaking::net::kakusan::control::VideoProfile{
            SourceStream::video_width, SourceStream::video_height,
            test_support::Bytes::unhex(SourceStream::video_header)});
    made.audio_header = test_support::Bytes::unhex_array<
        libchiaking::net::kakusan::control::audio_header_size>(
        SourceStream::audio_header);
    return made;
  }

  static auto shared_stream() -> libchiaking::play::SharedStream {
    libchiaking::play::SharedStream stream;
    stream.protocol_version = SourceStream::takion_version;
    stream.frames_per_second = SourceStream::video_fps;
    stream.description.profiles.push_back(libchiaking::media::VideoProfile{
        SourceStream::video_width, SourceStream::video_height,
        test_support::Bytes::unhex(SourceStream::video_header)});
    stream.description.audio_header = test_support::Bytes::unhex_array<
        libchiaking::media::StreamDescription::audio_header_size>(
        SourceStream::audio_header);
    return stream;
  }
};

}  // namespace kakusan_test
