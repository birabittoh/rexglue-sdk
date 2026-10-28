// rexglue - ASF movie decoder (WMV3 video + WMA audio)
//
// Decodes a title's .wmv movie for host-side playback: the video stream to
// RGBA frames in display order, the audio stream to PCM. Like WmaPlayer, it
// parses the ASF container itself, since the SDK's FFmpeg has no
// libavformat. It only decodes; showing the frames (and playing the PCM,
// e.g. with RawPcmPlayer) is up to the caller, which knows its renderer.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rex::audio {

class AsfMovieDecoder {
 public:
  AsfMovieDecoder();
  ~AsfMovieDecoder();

  AsfMovieDecoder(const AsfMovieDecoder&) = delete;
  AsfMovieDecoder& operator=(const AsfMovieDecoder&) = delete;

  // Parses |file| (a whole .wmv) and opens its video decoder. Returns false
  // if it isn't an ASF file with a WMV3 video stream.
  bool Open(std::vector<uint8_t> file);

  uint32_t width() const;
  uint32_t height() const;
  // Presentation length in seconds, from the ASF file properties.
  double duration() const;

  // Decodes the whole audio stream to interleaved 16-bit PCM. Returns false
  // if the movie has no WMA audio or it can't be decoded.
  bool DecodeAudio(std::vector<int16_t>& pcm, uint32_t& sample_rate, uint16_t& channels);

  // Decodes the next video frame in display order into |rgba|
  // (width() * height() * 4 bytes) and its presentation time in seconds.
  // Returns false once every frame has been returned.
  bool NextVideoFrame(std::vector<uint8_t>& rgba, double& pts);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::audio
