// rexglue - ASF movie decoder. See include/rex/audio/asf_movie_decoder.h.

#include <rex/audio/asf_movie_decoder.h>

#include <algorithm>
#include <cstring>
#include <map>

#include <rex/logging.h>

extern "C" {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/samplefmt.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

namespace {

// ASF object GUIDs (16-byte little-endian forms, as stored in the file).
constexpr uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                     0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidFileProps[16] = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidStreamProps[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                          0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidData[16] = {0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                   0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidAudioMedia[16] = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                         0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr uint8_t kGuidVideoMedia[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                         0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

constexpr uint32_t kFourccWmv3 = 0x33564D57;  // "WMV3"

bool GuidEq(const uint8_t* a, const uint8_t* b) {
  return std::memcmp(a, b, 16) == 0;
}

uint16_t Le16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) {
  return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32);
}

// Reads an ASF variable-length field. |type|: 0=>absent(0), 1=>byte, 2=>word, 3=>dword.
uint32_t ReadVar(const uint8_t* p, int type, size_t& cursor) {
  switch (type) {
    case 1:
      return p[cursor++];
    case 2: {
      uint16_t v = Le16(p + cursor);
      cursor += 2;
      return v;
    }
    case 3: {
      uint32_t v = Le32(p + cursor);
      cursor += 4;
      return v;
    }
    default:
      return 0;
  }
}

uint8_t ClampByte(int v) {
  return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

struct AsfMovieDecoder::State {
  // One reassembled media object of a stream.
  struct Packet {
    std::vector<uint8_t> data;
    uint32_t pts_ms = 0;
    bool key = false;
  };
  struct Stream {
    uint8_t number = 0;
    std::vector<uint8_t> extradata;
    std::vector<Packet> packets;
    // Media object being reassembled.
    Packet pending;
    size_t pending_size = 0;
    bool pending_active = false;
  };

  std::vector<uint8_t> file;
  size_t packet_size = 0;
  size_t data_packet_base = 0;
  size_t data_packet_count = 0;
  uint64_t preroll_ms = 0;
  double duration = 0.0;

  Stream video;
  uint32_t width = 0;
  uint32_t height = 0;

  Stream audio;
  uint16_t audio_format_tag = 0;
  uint16_t audio_channels = 0;
  uint32_t audio_sample_rate = 0;
  uint32_t audio_avg_bytes = 0;
  uint16_t audio_block_align = 0;
  uint16_t audio_bits = 0;

  AVCodecContext* video_ctx = nullptr;
  AVPacket* pkt = nullptr;
  AVFrame* frame = nullptr;
  std::vector<uint8_t> pkt_buf;
  size_t next_video_packet = 0;
  bool video_drained = false;

  ~State() {
    if (video_ctx)
      avcodec_free_context(&video_ctx);
    if (pkt)
      av_packet_free(&pkt);
    if (frame)
      av_frame_free(&frame);
  }

  bool ParseHeader();
  void DemuxPacket(size_t index);
  void Append(Stream& stream, const uint8_t* payload, uint32_t length, uint32_t offset,
              uint32_t object_size, uint32_t pts_ms, bool key);
  static void Flush(Stream& stream);
  bool OpenVideo();
  bool Send(AVCodecContext* ctx, const Packet& packet);
  void ToRgba(std::vector<uint8_t>& rgba) const;
};

bool AsfMovieDecoder::State::ParseHeader() {
  const uint8_t* d = file.data();
  const size_t n = file.size();
  if (n < 30 || !GuidEq(d, kGuidHeader)) {
    REXAPU_ERROR("AsfMovieDecoder: not an ASF file");
    return false;
  }
  const uint64_t header_size = Le64(d + 16);
  if (header_size < 30 || header_size > n) {
    REXAPU_ERROR("AsfMovieDecoder: bad ASF header size");
    return false;
  }

  for (size_t p = 30; p + 24 <= header_size;) {
    const uint8_t* g = d + p;
    const uint64_t obj_size = Le64(d + p + 16);
    if (obj_size < 24 || p + obj_size > header_size)
      break;
    const uint8_t* body = d + p + 24;
    const size_t body_size = static_cast<size_t>(obj_size - 24);

    if (GuidEq(g, kGuidFileProps) && body_size >= 72) {
      // FileID(16) FileSize(8) Created(8) Packets(8) PlayDuration(8)
      // SendDuration(8) Preroll(8) Flags(4) MinPacketSize(4) ...
      const uint64_t play_duration = Le64(body + 40);
      preroll_ms = Le64(body + 56);
      packet_size = Le32(body + 68);
      duration = std::max(0.0, play_duration / 1e7 - preroll_ms / 1e3);
    } else if (GuidEq(g, kGuidStreamProps) && body_size >= 54) {
      // StreamType(16) ECType(16) TimeOffset(8) TypeSpecLen(4) ECLen(4)
      // Flags(2) Reserved(4) TypeSpecific...
      const uint32_t spec_len = Le32(body + 40);
      const uint8_t number = uint8_t(Le16(body + 48) & 0x7F);
      const uint8_t* spec = body + 54;
      if (54 + size_t(spec_len) > body_size) {
        p += obj_size;
        continue;
      }
      if (GuidEq(body, kGuidVideoMedia) && spec_len >= 11 + 40 && !video.number) {
        // EncodedWidth(4) EncodedHeight(4) Reserved(1) FormatSize(2), then a
        // BITMAPINFOHEADER and the codec's extradata.
        const uint16_t format_size = Le16(spec + 9);
        const uint8_t* bih = spec + 11;
        if (format_size >= 40 && 11 + size_t(format_size) <= spec_len &&
            Le32(bih + 16) == kFourccWmv3) {
          video.number = number;
          width = Le32(bih + 4);
          height = Le32(bih + 8);
          video.extradata.assign(bih + 40, bih + format_size);
        }
      } else if (GuidEq(body, kGuidAudioMedia) && spec_len >= 18 && !audio.number) {
        // WAVEFORMATEX and its extradata.
        audio.number = number;
        audio_format_tag = Le16(spec + 0);
        audio_channels = Le16(spec + 2);
        audio_sample_rate = Le32(spec + 4);
        audio_avg_bytes = Le32(spec + 8);
        audio_block_align = Le16(spec + 12);
        audio_bits = Le16(spec + 14);
        const uint16_t cb = Le16(spec + 16);
        if (18 + size_t(cb) <= spec_len)
          audio.extradata.assign(spec + 18, spec + 18 + cb);
      }
    }
    p += obj_size;
  }

  if (!video.number || !width || !height) {
    REXAPU_ERROR("AsfMovieDecoder: no WMV3 video stream");
    return false;
  }
  if (!packet_size) {
    REXAPU_ERROR("AsfMovieDecoder: zero ASF packet size");
    return false;
  }

  // The data object follows the header object.
  const size_t data_off = static_cast<size_t>(header_size);
  if (data_off + 50 > n || !GuidEq(d + data_off, kGuidData)) {
    REXAPU_ERROR("AsfMovieDecoder: ASF data object not found");
    return false;
  }
  const uint64_t data_obj_size = Le64(d + data_off + 16);
  data_packet_base = data_off + 50;
  const uint64_t avail = (data_obj_size >= 50 && data_off + data_obj_size <= n)
                             ? data_obj_size - 50
                             : n - data_packet_base;
  data_packet_count = static_cast<size_t>(avail / packet_size);
  return data_packet_count != 0;
}

void AsfMovieDecoder::State::Append(Stream& stream, const uint8_t* payload, uint32_t length,
                                    uint32_t offset, uint32_t object_size, uint32_t pts_ms,
                                    bool key) {
  // A new media object starts at offset 0; an unfinished one before it is
  // kept as is.
  if (offset == 0) {
    Flush(stream);
    stream.pending.data.clear();
    stream.pending.pts_ms = pts_ms;
    stream.pending.key = key;
    stream.pending_size = object_size ? object_size : length;
    stream.pending_active = true;
  } else if (!stream.pending_active) {
    return;  // the rest of an object whose start was lost
  }
  stream.pending.data.insert(stream.pending.data.end(), payload, payload + length);
  if (stream.pending.data.size() >= stream.pending_size)
    Flush(stream);
}

void AsfMovieDecoder::State::Flush(Stream& stream) {
  if (stream.pending_active && !stream.pending.data.empty())
    stream.packets.push_back(std::move(stream.pending));
  stream.pending = {};
  stream.pending_active = false;
}

void AsfMovieDecoder::State::DemuxPacket(size_t index) {
  const uint8_t* d = file.data() + data_packet_base + index * packet_size;
  const size_t end = packet_size;
  size_t cur = 0;

  // Error-correction data (optional).
  if (d[0] & 0x80)
    cur += 1 + (d[0] & 0x0F);
  if (cur + 2 > end)
    return;
  const uint8_t length_type_flags = d[cur++];
  const uint8_t property_flags = d[cur++];

  const bool multiple = (length_type_flags & 0x01) != 0;
  const int seq_type = (length_type_flags >> 1) & 0x03;
  const int pad_len_type = (length_type_flags >> 3) & 0x03;
  const int pkt_len_type = (length_type_flags >> 5) & 0x03;
  const int repl_type = property_flags & 0x03;
  const int off_type = (property_flags >> 2) & 0x03;
  const int mon_type = (property_flags >> 4) & 0x03;

  ReadVar(d, pkt_len_type, cur);
  ReadVar(d, seq_type, cur);
  const uint32_t padding_length = ReadVar(d, pad_len_type, cur);
  cur += 4;  // send time
  cur += 2;  // duration

  int payload_count = 1;
  int payload_len_type = 0;
  if (multiple) {
    const uint8_t pf = d[cur++];
    payload_count = pf & 0x3F;
    payload_len_type = (pf >> 6) & 0x03;
  }
  const size_t payload_region_end = end - std::min<size_t>(padding_length, end);

  for (int i = 0; i < payload_count && cur < end; ++i) {
    const uint8_t stream_byte = d[cur++];
    ReadVar(d, mon_type, cur);  // media object number
    const uint32_t offset = ReadVar(d, off_type, cur);
    const uint32_t replicated_length = ReadVar(d, repl_type, cur);
    uint32_t object_size = 0;
    uint32_t pts_ms = 0;
    if (replicated_length >= 8 && cur + 8 <= end) {
      object_size = Le32(d + cur);
      pts_ms = Le32(d + cur + 4);
    }
    cur += replicated_length;
    const uint32_t length = multiple
                                ? ReadVar(d, payload_len_type, cur)
                                : uint32_t(payload_region_end > cur ? payload_region_end - cur : 0);
    if (cur + length > end)
      break;
    const uint8_t* payload = d + cur;
    cur += length;
    if (replicated_length == 1)
      continue;  // compressed payloads: unsupported

    const uint8_t number = stream_byte & 0x7F;
    const bool key = (stream_byte & 0x80) != 0;
    const uint32_t pts = pts_ms > preroll_ms ? uint32_t(pts_ms - preroll_ms) : 0;
    if (number == video.number)
      Append(video, payload, length, offset, object_size, pts, key);
    else if (audio.number && number == audio.number)
      Append(audio, payload, length, offset, object_size, pts, key);
  }
}

bool AsfMovieDecoder::State::OpenVideo() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!codec) {
    REXAPU_ERROR("AsfMovieDecoder: WMV3 decoder not available");
    return false;
  }
  video_ctx = avcodec_alloc_context3(codec);
  if (!video_ctx)
    return false;
  video_ctx->width = static_cast<int>(width);
  video_ctx->height = static_cast<int>(height);
  video_ctx->codec_tag = kFourccWmv3;
  video_ctx->pkt_timebase = AVRational{1, 1000};
  video_ctx->thread_count = 1;
  if (!video.extradata.empty()) {
    video_ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(video.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!video_ctx->extradata)
      return false;
    std::memcpy(video_ctx->extradata, video.extradata.data(), video.extradata.size());
    video_ctx->extradata_size = static_cast<int>(video.extradata.size());
  }
  if (avcodec_open2(video_ctx, codec, nullptr) < 0) {
    REXAPU_ERROR("AsfMovieDecoder: failed to open the WMV3 decoder");
    return false;
  }
  pkt = av_packet_alloc();
  frame = av_frame_alloc();
  return pkt && frame;
}

bool AsfMovieDecoder::State::Send(AVCodecContext* ctx, const Packet& packet) {
  // libavcodec reads past the end of packets; provide padding.
  pkt_buf.assign(packet.data.begin(), packet.data.end());
  pkt_buf.resize(packet.data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  av_packet_unref(pkt);
  pkt->data = pkt_buf.data();
  pkt->size = static_cast<int>(packet.data.size());
  pkt->pts = packet.pts_ms;
  pkt->dts = AV_NOPTS_VALUE;
  pkt->flags = packet.key ? AV_PKT_FLAG_KEY : 0;
  const int ret = avcodec_send_packet(ctx, pkt);
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_DEBUG("AsfMovieDecoder: send_packet error: {}", errbuf);
    return false;
  }
  return true;
}

// BT.601 limited range, the WMV default.
void AsfMovieDecoder::State::ToRgba(std::vector<uint8_t>& rgba) const {
  rgba.resize(size_t(width) * height * 4);
  const int w = std::min<int>(frame->width, static_cast<int>(width));
  const int h = std::min<int>(frame->height, static_cast<int>(height));
  const bool chroma_full_height =
      frame->format == AV_PIX_FMT_YUV422P || frame->format == AV_PIX_FMT_YUV444P;
  const bool chroma_full_width = frame->format == AV_PIX_FMT_YUV444P;
  for (int y = 0; y < h; ++y) {
    const uint8_t* py = frame->data[0] + y * frame->linesize[0];
    const int cy = chroma_full_height ? y : y / 2;
    const uint8_t* pu = frame->data[1] + cy * frame->linesize[1];
    const uint8_t* pv = frame->data[2] + cy * frame->linesize[2];
    uint8_t* out = rgba.data() + size_t(y) * width * 4;
    for (int x = 0; x < w; ++x) {
      const int cx = chroma_full_width ? x : x / 2;
      const int c = (py[x] - 16) * 298;
      const int u = pu[cx] - 128;
      const int v = pv[cx] - 128;
      out[x * 4 + 0] = ClampByte((c + 409 * v + 128) >> 8);
      out[x * 4 + 1] = ClampByte((c - 100 * u - 208 * v + 128) >> 8);
      out[x * 4 + 2] = ClampByte((c + 516 * u + 128) >> 8);
      out[x * 4 + 3] = 255;
    }
  }
}

AsfMovieDecoder::AsfMovieDecoder() = default;
AsfMovieDecoder::~AsfMovieDecoder() = default;

bool AsfMovieDecoder::Open(std::vector<uint8_t> file) {
  state_ = std::make_unique<State>();
  state_->file = std::move(file);
  if (!state_->ParseHeader()) {
    state_.reset();
    return false;
  }
  for (size_t i = 0; i < state_->data_packet_count; ++i)
    state_->DemuxPacket(i);
  State::Flush(state_->video);
  State::Flush(state_->audio);
  if (!state_->OpenVideo()) {
    state_.reset();
    return false;
  }
  REXAPU_INFO("AsfMovieDecoder: {}x{}, {:.2f} s, {} video and {} audio packets", state_->width,
              state_->height, state_->duration, state_->video.packets.size(),
              state_->audio.packets.size());
  return true;
}

uint32_t AsfMovieDecoder::width() const {
  return state_ ? state_->width : 0;
}

uint32_t AsfMovieDecoder::height() const {
  return state_ ? state_->height : 0;
}

double AsfMovieDecoder::duration() const {
  return state_ ? state_->duration : 0.0;
}

bool AsfMovieDecoder::DecodeAudio(std::vector<int16_t>& pcm, uint32_t& sample_rate,
                                  uint16_t& channels) {
  pcm.clear();
  if (!state_ || !state_->audio.number || state_->audio.packets.empty())
    return false;
  const State& s = *state_;
  const AVCodecID id = s.audio_format_tag == 0x0162 || s.audio_format_tag == 0x0163
                           ? AV_CODEC_ID_WMAPRO
                           : AV_CODEC_ID_WMAV2;
  const AVCodec* codec = avcodec_find_decoder(id);
  if (!codec)
    return false;
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx)
    return false;
  ctx->channels = s.audio_channels;
  ctx->sample_rate = static_cast<int>(s.audio_sample_rate);
  ctx->block_align = s.audio_block_align;
  ctx->bit_rate = static_cast<int64_t>(s.audio_avg_bytes) * 8;
  ctx->bits_per_coded_sample = s.audio_bits;
  if (!s.audio.extradata.empty()) {
    ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(s.audio.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (ctx->extradata) {
      std::memcpy(ctx->extradata, s.audio.extradata.data(), s.audio.extradata.size());
      ctx->extradata_size = static_cast<int>(s.audio.extradata.size());
    }
  }
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }

  const int out_channels = ctx->channels > 0 ? ctx->channels : 1;
  auto drain = [&]() {
    while (avcodec_receive_frame(ctx, state_->frame) == 0) {
      AVFrame* f = state_->frame;
      const auto fmt = static_cast<AVSampleFormat>(f->format);
      const bool planar = av_sample_fmt_is_planar(fmt) != 0;
      const size_t base = pcm.size();
      pcm.resize(base + size_t(f->nb_samples) * out_channels);
      for (int c = 0; c < out_channels; ++c) {
        const uint8_t* src = planar ? f->data[c] : f->data[0];
        for (int i = 0; i < f->nb_samples; ++i) {
          float v = 0.0f;
          switch (fmt) {
            case AV_SAMPLE_FMT_FLTP:
              v = reinterpret_cast<const float*>(src)[i];
              break;
            case AV_SAMPLE_FMT_FLT:
              v = reinterpret_cast<const float*>(src)[i * out_channels + c];
              break;
            case AV_SAMPLE_FMT_S16P:
              v = reinterpret_cast<const int16_t*>(src)[i] / 32768.0f;
              break;
            case AV_SAMPLE_FMT_S16:
              v = reinterpret_cast<const int16_t*>(src)[i * out_channels + c] / 32768.0f;
              break;
            default:
              break;
          }
          pcm[base + size_t(i) * out_channels + c] =
              static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        }
      }
      av_frame_unref(f);
    }
  };
  for (const State::Packet& packet : s.audio.packets) {
    state_->Send(ctx, packet);
    drain();
  }
  avcodec_send_packet(ctx, nullptr);
  drain();

  sample_rate = static_cast<uint32_t>(ctx->sample_rate);
  channels = static_cast<uint16_t>(out_channels);
  avcodec_free_context(&ctx);
  return !pcm.empty();
}

bool AsfMovieDecoder::NextVideoFrame(std::vector<uint8_t>& rgba, double& pts) {
  if (!state_)
    return false;
  State& s = *state_;
  while (true) {
    const int ret = avcodec_receive_frame(s.video_ctx, s.frame);
    if (ret == 0) {
      const int64_t ts = s.frame->best_effort_timestamp != AV_NOPTS_VALUE
                             ? s.frame->best_effort_timestamp
                             : s.frame->pts;
      pts = ts != AV_NOPTS_VALUE ? ts / 1000.0 : 0.0;
      s.ToRgba(rgba);
      av_frame_unref(s.frame);
      return true;
    }
    if (ret == AVERROR_EOF || s.video_drained)
      return false;
    if (s.next_video_packet < s.video.packets.size()) {
      s.Send(s.video_ctx, s.video.packets[s.next_video_packet++]);
    } else {
      avcodec_send_packet(s.video_ctx, nullptr);
      s.video_drained = true;
    }
  }
}

}  // namespace rex::audio
