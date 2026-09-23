/**
 * @file        rex/ui/progress_window.h
 * @brief       Standalone SDL progress window for long blocking startup steps.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace rex::ui {

/// Colors for ProgressWindow, as {R, G, B} bytes. Defaults match the SDK's own
/// neutral dark theme.
struct ProgressWindowTheme {
  uint8_t background[3] = {18, 18, 22};
  uint8_t bar_fill[3] = {90, 160, 240};
  uint8_t bar_frame[3] = {90, 90, 100};
  uint8_t title_text[3] = {230, 230, 235};
  uint8_t detail_text[3] = {150, 150, 160};
};

/// A throwaway window with a progress bar, for work that blocks the UI thread
/// before the app has a window of its own (game data extraction). It opens at
/// the size and mode the app window is about to use, so the hand over is not
/// jarring, and lives only as long as the step it reports on.
///
/// Everything degrades gracefully: if the window or renderer cannot be created,
/// ok() is false and Draw() only keeps the thread responsive.
class ProgressWindow {
 public:
  /// `icon_data` is encoded image bytes (anything DecodeImageRGBA accepts),
  /// only read during construction. Null shows no icon.
  explicit ProgressWindow(const std::string& window_title = "Preparing game files",
                          const ProgressWindowTheme& theme = {}, const void* icon_data = nullptr,
                          size_t icon_size = 0);
  ~ProgressWindow();

  ProgressWindow(const ProgressWindow&) = delete;
  ProgressWindow& operator=(const ProgressWindow&) = delete;

  bool ok() const { return renderer_ != nullptr; }

  /// Draw one frame. `fraction` outside [0,1] means "total unknown", which
  /// draws a block sweeping back and forth instead of a filling bar.
  void Draw(const std::string& title, float fraction, const std::string& detail);

 private:
  ProgressWindowTheme theme_;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* icon_texture_ = nullptr;
  bool owns_video_ = false;
};

/// Pumps OS events for a UI thread that is busy with a long step, so none of
/// its windows is marked unresponsive. Events stay queued for the app's own
/// loop; event watches (app lifecycle, quit) still run here.
void PumpPlatformEvents();

}  // namespace rex::ui
