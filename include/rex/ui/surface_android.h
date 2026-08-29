#pragma once
/**
 * @file        ui/surface_android.h
 * @brief       Android ANativeWindow surface for Vulkan presentation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/surface.h>

#include <android/native_window.h>

struct SDL_Window;

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window, SDL_Window* sdl_window)
      : window_(window), sdl_window_(sdl_window) {}

  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
  SDL_Window* sdl_window_;
};

}  // namespace ui
}  // namespace rex
