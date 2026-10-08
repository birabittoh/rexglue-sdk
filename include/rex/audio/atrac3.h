// rexglue - ATRAC3 decoding through the vendored FFmpeg, for tools that
// convert PS3 audio and have no other codec at hand.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::audio {

// Decodes back to back ATRAC3 frames of `block_align` bytes (192 for the
// usual mono 66 kbps) into interleaved 16 bit PCM, 1024 samples per channel
// per frame. False if FFmpeg refuses the stream; `out` then holds what
// decoded before the failure.
bool DecodeAtrac3(const uint8_t* frames, size_t size, uint32_t sample_rate, uint16_t channels,
                  uint16_t block_align, std::vector<int16_t>& out);

}  // namespace rex::audio
