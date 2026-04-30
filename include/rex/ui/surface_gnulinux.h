#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>

#include <rex/platform.h>
#include <rex/ui/surface.h>

#include <xcb/xcb.h>

#if REX_HAS_WAYLAND
#include <wayland-client.h>
#endif

namespace rex {
namespace ui {

class XcbWindowSurface final : public Surface {
 public:
  explicit XcbWindowSurface(xcb_connection_t* connection, xcb_window_t window)
      : connection_(connection), window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_XcbWindow; }
  xcb_connection_t* connection() const { return connection_; }
  xcb_window_t window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  xcb_connection_t* connection_;
  xcb_window_t window_;
};

#if REX_HAS_WAYLAND

class WaylandWindowSurface final : public Surface {
 public:
  explicit WaylandWindowSurface(wl_display* display, wl_surface* surface,
                                uint32_t width, uint32_t height)
      : display_(display), surface_(surface), width_(width), height_(height) {}
  TypeIndex GetType() const override { return kTypeIndex_WaylandSurface; }
  wl_display* display() const { return display_; }
  wl_surface* surface() const { return surface_; }
  void SetSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
  }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  wl_display* display_;
  wl_surface* surface_;
  uint32_t width_;
  uint32_t height_;
};

#endif  // REX_HAS_WAYLAND

}  // namespace ui
}  // namespace rex
