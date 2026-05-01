/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <string>

#include <rex/platform.h>
#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <xcb/xcb.h>

namespace rex {
namespace ui {

#if REX_HAS_WAYLAND
class WaylandWindowSurface;
#endif

class GTKWindow : public Window {
  using super = Window;

 public:
  GTKWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~GTKWindow() override;

  // Will be null if the window hasn't been successfully opened yet, or has been
  // closed.
  GtkWidget* window() const { return window_; }

  // Warp the pointer to the given position in the client (drawing) area.
  // Returns true if the warp was performed (X11), false if it's a no-op
  // (Wayland).
  bool WarpPointer(int32_t x, int32_t y);

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewMainMenu(MenuItem* old_main_menu) override;
  // Mouse capture seems to happen implicitly compared to Windows.
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;

 private:
  void HandleSizeUpdate(WindowDestructionReceiver& destruction_receiver);
  // For updating multiple factors that may influence the window size at once,
  // without handling the configure event multiple times (that may not only
  // result in wasted handling, but also in the state potentially changed to an
  // inconsistent one in the middle of a size update by the listeners).
  void BeginBatchedSizeUpdate();
  void EndBatchedSizeUpdate(WindowDestructionReceiver& destruction_receiver);

  // Handling events related to the whole window.
  bool HandleMouse(GdkEvent* event, WindowDestructionReceiver& destruction_receiver);
  bool HandleKeyboard(GdkEventKey* event, WindowDestructionReceiver& destruction_receiver);
  gboolean WindowEventHandler(GdkEvent* event);
  static gboolean WindowEventHandlerThunk(GtkWidget* widget, GdkEvent* event, gpointer user_data);

  // Handling events related specifically to the drawing (client) area.
  gboolean DrawingAreaEventHandler(GdkEvent* event);
  static gboolean DrawingAreaEventHandlerThunk(GtkWidget* widget, GdkEvent* event,
                                               gpointer user_data);
  static gboolean DrawHandler(GtkWidget* widget, cairo_t* cr, gpointer data);

  // Non-owning (initially floating) references to the widgets.
  GtkWidget* window_ = nullptr;
  GtkWidget* box_ = nullptr;
  GtkWidget* drawing_area_ = nullptr;

  uint32_t batched_size_update_depth_ = 0;
  bool batched_size_update_contained_configure_ = false;
  bool batched_size_update_contained_draw_ = false;

  // Cursor management.
  GdkCursor* blank_cursor_ = nullptr;
  bool pointer_grabbed_ = false;

#if REX_HAS_WAYLAND
  // Non-owning pointer to the active Wayland surface, kept in sync with
  // presenter_surface_ so HandleSizeUpdate can push physical pixel dimensions.
  WaylandWindowSurface* wayland_surface_ = nullptr;
  // Wayland globals needed for subsurface creation. Bound once on first use.
  struct wl_compositor* wl_compositor_ = nullptr;
  struct wl_subcompositor* wl_subcompositor_ = nullptr;
  bool wayland_globals_bound_ = false;
  void EnsureWaylandGlobals(struct wl_display* display);
#endif
};

class GTKMenuItem : public MenuItem {
 public:
  GTKMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback);
  ~GTKMenuItem() override;

  GtkWidget* handle() const { return menu_; }

 protected:
  void OnChildAdded(MenuItem* child_item) override;
  void OnChildRemoved(MenuItem* child_item) override;

 private:
  static void ActivateHandler(GtkWidget* menu_item, gpointer user_data);

  // An owning reference because a menu may be transferred between windows.
  GtkWidget* menu_ = nullptr;
};

}  // namespace ui
}  // namespace rex
