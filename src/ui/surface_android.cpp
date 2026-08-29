/**
 * @file        ui/surface_android.cpp
 * @brief       Android ANativeWindow surface implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/surface_android.h>

#include <SDL3/SDL.h>
#include <android/native_window.h>

namespace rex {
namespace ui {

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  // Prefer SDL's pixel size (accounts for DPI scaling) when the SDL window is
  // available; fall back to ANativeWindow directly.
  if (sdl_window_) {
    int w = 0, h = 0;
    if (SDL_GetWindowSizeInPixels(sdl_window_, &w, &h) && w > 0 && h > 0) {
      width_out = static_cast<uint32_t>(w);
      height_out = static_cast<uint32_t>(h);
      return true;
    }
  }
  if (window_) {
    int32_t w = ANativeWindow_getWidth(window_);
    int32_t h = ANativeWindow_getHeight(window_);
    if (w > 0 && h > 0) {
      width_out = static_cast<uint32_t>(w);
      height_out = static_cast<uint32_t>(h);
      return true;
    }
  }
  return false;
}

}  // namespace ui
}  // namespace rex
