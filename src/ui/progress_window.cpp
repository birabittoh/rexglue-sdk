/**
 * @file        ui/progress_window.cpp
 * @brief       Standalone SDL progress window for long blocking startup steps.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/progress_window.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/flags.h>
#include <rex/ui/image_decode.h>

namespace rex::ui {

namespace {

// The app window's own logical size, see ReXApp::SetupPresentation.
constexpr int kLogicalWidth = 1280;
constexpr int kLogicalHeight = 720;

}  // namespace

ProgressWindow::ProgressWindow(const std::string& window_title, const ProgressWindowTheme& theme,
                               const void* icon_data, size_t icon_size)
    : theme_(theme) {
  if (!SDL_WasInit(SDL_INIT_VIDEO)) {
    // Normally the windowed-app context already owns the video subsystem;
    // only claim it when nothing has (a consumer driving the selector on its
    // own), and hand it back in the destructor.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
      REXLOG_WARN("Progress window: SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
      return;
    }
    owns_video_ = true;
  }

  // Same size and mode the app window is about to open with. Android ignores
  // the size and fills the screen.
  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (REXCVAR_GET(fullscreen)) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }
  float scale = 1.0f;
#if !REX_PLATFORM_MAC
  // SDL window coordinates are physical pixels outside Cocoa.
  scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (scale <= 0.0f) {
    scale = 1.0f;
  }
#endif
  window_ = SDL_CreateWindow(window_title.c_str(), int(kLogicalWidth * scale),
                             int(kLogicalHeight * scale), flags);
  if (!window_) {
    REXLOG_WARN("Progress window: SDL_CreateWindow failed: {}", SDL_GetError());
    return;
  }
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) {
    REXLOG_WARN("Progress window: SDL_CreateRenderer failed: {}", SDL_GetError());
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    return;
  }
  REXLOG_INFO("Progress window up, using the '{}' render backend", SDL_GetRendererName(renderer_));

  if (icon_data && icon_size) {
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> rgba =
        DecodeImageRGBA(static_cast<const uint8_t*>(icon_data), icon_size, icon_w, icon_h);
    if (!rgba.empty()) {
      SDL_Surface* surface =
          SDL_CreateSurfaceFrom(icon_w, icon_h, SDL_PIXELFORMAT_RGBA32, rgba.data(), icon_w * 4);
      if (surface) {
        icon_texture_ = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (icon_texture_) {
          SDL_SetTextureScaleMode(icon_texture_, SDL_SCALEMODE_LINEAR);
        }
      }
    } else {
      REXLOG_WARN("Progress window: failed to decode icon image");
    }
  }
}

ProgressWindow::~ProgressWindow() {
  if (icon_texture_) {
    SDL_DestroyTexture(icon_texture_);
  }
  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
  }
  if (window_) {
    SDL_DestroyWindow(window_);
  }
  if (owns_video_) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
}

void PumpPlatformEvents() {
  SDL_PumpEvents();
}

void ProgressWindow::Draw(const std::string& title, float fraction, const std::string& detail) {
  // Without pumping, the window never paints and on desktop the OS marks every
  // window on this thread unresponsive. The events themselves stay queued for
  // the app's loop, so a close request still lands once the step is over.
  PumpPlatformEvents();
  if (!ok()) {
    return;
  }

  int out_w = 0, out_h = 0;
  SDL_GetRenderOutputSize(renderer_, &out_w, &out_h);
  if (out_w <= 0 || out_h <= 0) {
    return;
  }
  // The debug font is 8 px tall, which is unreadable on a phone panel or a
  // fullscreen window, so everything is drawn in logical units scaled up on
  // big surfaces, keeping room for a 60 character detail line.
  const float scale = std::max(1.0f, std::floor(std::min(static_cast<float>(out_h) / 220.0f,
                                                         static_cast<float>(out_w) / 520.0f)));
  SDL_SetRenderScale(renderer_, scale, scale);
  const float w = static_cast<float>(out_w) / scale;
  const float h = static_cast<float>(out_h) / scale;

  const auto& bg = theme_.background;
  const auto& fill_color = theme_.bar_fill;
  const auto& frame_color = theme_.bar_frame;
  const auto& title_color = theme_.title_text;
  const auto& detail_color = theme_.detail_text;

  SDL_SetRenderDrawColor(renderer_, bg[0], bg[1], bg[2], 255);
  SDL_RenderClear(renderer_);

  const float bar_w = std::min(w - 40.0f, 480.0f);
  const float bar_x = (w - bar_w) / 2.0f;
  const float bar_h = 18.0f;

  // The icon sits above the bar and pushes it down to make room, rather
  // than overlapping the vertical center used when there is no icon.
  float bar_y = h / 2.0f - bar_h / 2.0f;
  if (icon_texture_) {
    const float icon_size = std::min(h * 0.35f, 96.0f);
    bar_y = h / 2.0f - bar_h / 2.0f + icon_size * 0.5f + 18.0f;
    const SDL_FRect icon_rect{(w - icon_size) / 2.0f, bar_y - icon_size - 44.0f, icon_size,
                              icon_size};
    SDL_RenderTexture(renderer_, icon_texture_, nullptr, &icon_rect);
  }

  SDL_SetRenderDrawColor(renderer_, title_color[0], title_color[1], title_color[2], 255);
  SDL_RenderDebugText(renderer_, bar_x, bar_y - 26.0f, title.c_str());
  if (!detail.empty()) {
    SDL_SetRenderDrawColor(renderer_, detail_color[0], detail_color[1], detail_color[2], 255);
    SDL_RenderDebugText(renderer_, bar_x, bar_y + bar_h + 12.0f, detail.c_str());
  }

  SDL_FRect frame{bar_x, bar_y, bar_w, bar_h};
  SDL_SetRenderDrawColor(renderer_, frame_color[0], frame_color[1], frame_color[2], 255);
  SDL_RenderRect(renderer_, &frame);

  SDL_SetRenderDrawColor(renderer_, fill_color[0], fill_color[1], fill_color[2], 255);
  if (fraction >= 0.0f && fraction <= 1.0f) {
    SDL_FRect fill{bar_x + 2.0f, bar_y + 2.0f, (bar_w - 4.0f) * fraction, bar_h - 4.0f};
    SDL_RenderFillRect(renderer_, &fill);
  } else {
    // Indeterminate: a block bouncing on a two second period.
    const float block = (bar_w - 4.0f) * 0.25f;
    const float t = static_cast<float>(SDL_GetTicks() % 2000) / 1000.0f;  // 0..2
    const float travel = (bar_w - 4.0f) - block;
    const float x = travel * (t <= 1.0f ? t : 2.0f - t);
    SDL_FRect fill{bar_x + 2.0f + x, bar_y + 2.0f, block, bar_h - 4.0f};
    SDL_RenderFillRect(renderer_, &fill);
  }

  SDL_RenderPresent(renderer_);
}

}  // namespace rex::ui
