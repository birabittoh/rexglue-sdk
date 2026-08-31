/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_DOUBLE(audio_volume, 1.0, "Audio", "Master volume (0.0 - 1.0)").range(0.0, 1.0);

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  // Pick the channel count from the device *before* opening it. The spec handed to
  // SDL_OpenAudioDeviceStream() is what the physical device gets opened with when nothing
  // else has claimed it, so asking for 5.1 up front and inspecting the result afterwards
  // only tells us what we asked for. WASAPI happens to overrule the request with the real
  // mix format, which is why querying after the open used to work on Windows, but AAudio
  // hands back a 6 channel stream on a stereo phone speaker: Android builds an index
  // channel mask (0x8000003F) for it, which its mixer will not fold down to the speaker,
  // so every frame we submit is dropped and the game plays silently.
  SDL_AudioSpec device_spec = {};
  uint8_t device_channels = 2;
  if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_spec, NULL)) {
    // 5.1 only when the device really has the speakers for it; anything else gets the
    // stereo downmix, which is also the only other layout conversion.h can produce.
    device_channels = device_spec.channels >= static_cast<int>(frame_channels_)
                          ? static_cast<uint8_t>(frame_channels_)
                          : uint8_t{2};
  } else {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed, assuming stereo output: {}", SDL_GetError());
  }

  SDL_AudioSpec desired_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = device_channels;
  sdl_device_channels_ = device_channels;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  // The backend can still land on a layout of its own choosing. SDL resamples and remaps
  // between the stream and the device, so the only thing that has to agree with the device
  // is our own conversion step, and that is driven by what we asked for above, not by this.
  SDL_AudioSpec obtained_spec = desired_spec;
  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
        double volume = REXCVAR_GET(audio_volume);
        // Bypass the multiply at unity gain (the common case), since floating
        // point rounding from a redundant *1.0 isn't free across this many
        // samples every callback.
        if (volume < 1.0) {
          float scale = static_cast<float>(std::clamp(volume, 0.0, 1.0));
          for (int i = 0; i < sample_count; ++i) {
            data[i] *= scale;
          }
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
