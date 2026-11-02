#include "src/core/net/kakusan/controller_frame.hpp"

#include "src/core/utils/bytes/byte_io.hpp"

namespace libchiaking::net::kakusan {

void ControllerFrameCodec::write_header(bytes::Writer& writer,
                                        std::string_view prefix,
                                        std::uint8_t kind,
                                        const SeatBinding& binding,
                                        std::uint32_t sequence,
                                        std::uint32_t elapsed_ms) {
  static_cast<void>(writer.text(prefix) && writer.u8(frame_version) &&
                    writer.u8(kind) && writer.u8(binding.seat) &&
                    writer.u8(0) && writer.u32_be(binding.generation) &&
                    writer.u32_be(sequence) && writer.u32_be(elapsed_ms));
}

auto ControllerFrameCodec::read_header(bytes::Reader& reader,
                                       std::string_view prefix,
                                       std::uint8_t kind) noexcept
    -> Result<Header> {
  if (!reader.consume(prefix)) return fail(Error::protocol);
  const auto version = reader.u8().value();
  const auto found_kind = reader.u8().value();
  Header header;
  header.binding.seat = reader.u8().value();
  const auto reserved = reader.u8().value();
  header.binding.generation = reader.u32_be().value();
  header.sequence = reader.u32_be().value();
  header.elapsed_ms = reader.u32_be().value();
  if (version != frame_version || found_kind != kind || reserved != 0 ||
      header.binding.seat == 0 || header.binding.generation == 0)
    return fail(Error::protocol);
  return header;
}

auto ControllerFrameCodec::validate_input_state(
    const InputState& state) noexcept -> bool {
  if ((state.buttons & ~button_mask) != 0 || !valid_axis(state.left_x) ||
      !valid_axis(state.left_y) || !valid_axis(state.right_x) ||
      !valid_axis(state.right_y))
    return false;
  if (state.touch.active) return state.touch.id <= max_touch_id;
  return state.touch.id == 0 && state.touch.x == 0 && state.touch.y == 0;
}

auto ControllerFrameCodec::encode_input(const InputFrame& frame)
    -> Result<InputBytes> {
  if (!validate_input_state(frame.state) || frame.binding.seat == 0 ||
      frame.binding.generation == 0)
    return fail(Error::protocol);
  InputBytes output{};
  bytes::Writer writer{output};
  write_header(writer, input_prefix, kind_input, frame.binding, frame.sequence,
               frame.elapsed_ms);
  const auto& state = frame.state;
  static_cast<void>(
      writer.u32_be(state.buttons) && writer.i16_be(state.left_x) &&
      writer.i16_be(state.left_y) && writer.i16_be(state.right_x) &&
      writer.i16_be(state.right_y) && writer.u16_be(state.left_trigger) &&
      writer.u16_be(state.right_trigger) &&
      writer.u8(state.touch.active ? 1 : 0) && writer.u8(state.touch.id) &&
      writer.u16_be(state.touch.x) && writer.u16_be(state.touch.y) &&
      writer.u16_be(0));
  return output;
}

auto ControllerFrameCodec::decode_input(std::span<const std::byte> record)
    -> Result<InputFrame> {
  if (record.size() != input_frame_size) return fail(Error::protocol);
  bytes::Reader reader{record};
  const auto header = read_header(reader, input_prefix, kind_input);
  if (!header) return fail(header.error());
  InputFrame frame{header.value().binding,
                   header.value().sequence,
                   header.value().elapsed_ms,
                   {}};
  auto& state = frame.state;
  state.buttons = reader.u32_be().value();
  state.left_x = reader.i16_be().value();
  state.left_y = reader.i16_be().value();
  state.right_x = reader.i16_be().value();
  state.right_y = reader.i16_be().value();
  state.left_trigger = reader.u16_be().value();
  state.right_trigger = reader.u16_be().value();
  const auto active = reader.u8().value();
  state.touch.id = reader.u8().value();
  state.touch.x = reader.u16_be().value();
  state.touch.y = reader.u16_be().value();
  const auto reserved = reader.u16_be().value();
  if (active > 1 || reserved != 0) return fail(Error::protocol);
  state.touch.active = active == 1;
  if (!validate_input_state(state)) return fail(Error::protocol);
  return frame;
}

auto ControllerFrameCodec::encode_feedback(const FeedbackFrame& frame)
    -> Result<FeedbackBytes> {
  if (frame.binding.seat == 0 || frame.binding.generation == 0)
    return fail(Error::protocol);
  FeedbackBytes output{};
  bytes::Writer writer{output};
  write_header(writer, feedback_prefix, kind_feedback, frame.binding,
               frame.sequence, frame.elapsed_ms);
  static_cast<void>(writer.u16_be(frame.low_frequency) &&
                    writer.u16_be(frame.high_frequency));
  return output;
}

auto ControllerFrameCodec::decode_feedback(std::span<const std::byte> record)
    -> Result<FeedbackFrame> {
  if (record.size() != feedback_frame_size) return fail(Error::protocol);
  bytes::Reader reader{record};
  const auto header = read_header(reader, feedback_prefix, kind_feedback);
  if (!header) return fail(header.error());
  const auto low_frequency = reader.u16_be().value();
  const auto high_frequency = reader.u16_be().value();
  return FeedbackFrame{header.value().binding, header.value().sequence,
                       header.value().elapsed_ms, low_frequency,
                       high_frequency};
}

}  // namespace libchiaking::net::kakusan
