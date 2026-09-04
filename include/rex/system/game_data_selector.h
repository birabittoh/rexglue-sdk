#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rex::system {

/// Colors for the progress window shown during extraction, as {R, G, B}
/// bytes. Defaults match the SDK's own neutral dark theme; a guest app can
/// retheme it to match its own overlay without touching SDL directly.
struct ProgressWindowTheme {
  uint8_t background[3] = {18, 18, 22};
  uint8_t bar_fill[3] = {90, 160, 240};
  uint8_t bar_frame[3] = {90, 90, 100};
  uint8_t title_text[3] = {230, 230, 235};
  uint8_t detail_text[3] = {150, 150, 160};
};

/// Configuration for GameDataSelector, set by the guest application.
struct GameDataSelectorSettings {
  /// Whether to accept only Xbox Live Arcade (XBLA) packages. When false
  /// (default), only ISO is accepted.
  bool is_xbla = false;
  /// Optional SHA-256 hex string. If non-empty, default.xex inside the game
  /// data *must* match this hash or the module fails with a message box.
  std::string default_xex_sha256;

  /// SHA-256 hex string for the title-update package file. If non-empty, a
  /// title update is required.
  std::string title_update_sha256;

  /// Config file the resolved game_data_root is written back to, so the user
  /// is only asked once. Pass the app's config_path(). When empty, the path
  /// is derived from the executable name (`<exe stem>.toml` next to the
  /// executable).
  std::filesystem::path config_path;

  /// Colors for the extraction progress window. Defaults to the SDK's
  /// neutral dark theme.
  ProgressWindowTheme progress_theme;

  /// Optional icon drawn above the progress bar while extraction runs, as
  /// encoded image bytes (PNG, etc. — anything rex::ui::DecodeImageRGBA
  /// accepts). Not owned; must outlive the EnsureGameData call. Left null to
  /// show no icon.
  const void* progress_icon_data = nullptr;
  size_t progress_icon_size = 0;
};

/// Synchronous startup wizard that runs BEFORE any window or presenter is
/// created. Uses native SDL dialogs (message boxes + file open dialog) to
/// prompt the user for game files (ISO / XBLA / already-extracted directory),
/// extracts them if needed, and validates default.xex against an optional
/// expected SHA-256.
///
/// Designed to be called from an overridden SetupEnvironment() in the guest
/// app, after the base SetupEnvironment has loaded the config and initialized
/// logging.
///
/// Usage:
/// @code
///   bool SetupEnvironment() override {
///     if (!rex::ReXApp::SetupEnvironment()) return false;
///     rex::system::GameDataSelectorSettings settings;
///     settings.default_xex_sha256 = "aabb...";
///     settings.title_update_sha256 = "ccdd...";
///     return rex::system::GameDataSelector::EnsureGameData(settings);
///   }
/// @endcode
class GameDataSelector {
 public:
  /// Checks game_data_root via the cvar. If already valid (non-empty path,
  /// default.xex present, hash matches), returns true immediately.
  /// Otherwise shows native SDL dialogs to let the user browse for an ISO,
  /// XBLA, or an already-extracted directory, extracts the game data, and
  /// sets game_data_root before returning.
  ///
  /// Returns true if game data is ready, false if the user cancelled.
  static bool EnsureGameData(const GameDataSelectorSettings& settings);
};

}  // namespace rex::system