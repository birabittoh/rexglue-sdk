// rexglue - ATRAC3 decoding. See include/rex/audio/atrac3.h.

#include <rex/audio/atrac3.h>

#include <cmath>
#include <cstring>

#include <rex/logging.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

namespace {

int16_t ToInt16(float v) {
  const long s = std::lrintf(v * 32768.0f);
  return int16_t(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}

}  // namespace

bool DecodeAtrac3(const uint8_t* frames, size_t size, uint32_t sample_rate, uint16_t channels,
                  uint16_t block_align, std::vector<int16_t>& out) {
  out.clear();
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3);
  if (!codec || !channels || !block_align)
    return false;
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = ctx && packet && frame;
  if (ok) {
    ctx->sample_rate = int(sample_rate);
    ctx->channels = channels;
    ctx->channel_layout = av_get_default_channel_layout(channels);
    ctx->block_align = block_align;
    // The 14 byte WAVEFORMATEX extension a RIFF ATRAC3 file carries: version
    // 1, no joint stereo, frame factor 1.
    static const uint8_t kExtradata[14] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(sizeof(kExtradata) + AV_INPUT_BUFFER_PADDING_SIZE));
    ok = ctx->extradata != nullptr;
    if (ok) {
      std::memcpy(ctx->extradata, kExtradata, sizeof(kExtradata));
      ctx->extradata_size = sizeof(kExtradata);
      ok = avcodec_open2(ctx, codec, nullptr) >= 0;
    }
  }
  std::vector<uint8_t> block(size_t(block_align) + AV_INPUT_BUFFER_PADDING_SIZE);
  for (size_t at = 0; ok && at + block_align <= size; at += block_align) {
    std::memcpy(block.data(), frames + at, block_align);
    packet->data = block.data();
    packet->size = block_align;
    if (avcodec_send_packet(ctx, packet) < 0) {
      ok = false;
      break;
    }
    while (avcodec_receive_frame(ctx, frame) >= 0) {
      const int n = frame->nb_samples;
      const int ch = frame->channels;
      for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch; ++c) {
          const float* plane = reinterpret_cast<const float*>(
              frame->format == AV_SAMPLE_FMT_FLTP ? frame->extended_data[c] : frame->data[0]);
          out.push_back(
              ToInt16(frame->format == AV_SAMPLE_FMT_FLTP ? plane[i] : plane[i * ch + c]));
        }
      }
    }
  }
  if (!ok)
    REXAPU_ERROR("ATRAC3: decoding failed after {} samples", out.size());
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  return ok;
}

}  // namespace rex::audio
