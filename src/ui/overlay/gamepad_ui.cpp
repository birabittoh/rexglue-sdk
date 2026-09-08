/**
 * @file        ui/overlay/gamepad_ui.cpp
 * @brief       Gamepad UI controller implementation. See gamepad_ui.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/gamepad_ui.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

namespace rex::ui {

namespace {

constexpr int16_t kStickDeadzone = 8000;  // ~X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE
constexpr uint8_t kTriggerThreshold = 30;
constexpr float kWindowMoveSpeed = 500.0f;    // px/sec at full stick deflection
constexpr float kWindowResizeSpeed = 500.0f;  // px/sec at full stick deflection
constexpr float kFlashDisplaySeconds = 2.0f;

float NormalizeAxis(int16_t v) {
  if (v > -kStickDeadzone && v < kStickDeadzone) {
    return 0.0f;
  }
  return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f);
}

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

GamepadUiController::GamepadUiController(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime)
    : ImGuiDialog(imgui_drawer), runtime_(runtime) {
  // Guide is normally stripped from every guest read (see xam_input.cpp) and
  // only delivered to the host at all when this passthrough cvar is set --
  // enable it unconditionally so this controller can read guide regardless
  // of what the user has configured for in-game guide behavior.
  REXCVAR_SET(guide_button, true);

  // Keyboard fallback: guide is frequently intercepted by Steam/the OS
  // before it ever reaches the game, so a controller-only trigger would be a
  // dead end for anyone hitting that. No is_visible/window_title: this
  // bind doesn't toggle a single overlay, it toggles the whole input mode.
  // Only sets a flag here rather than calling ToggleMode() directly; see
  // pending_keyboard_toggle_'s comment in the header for why.
  rex::ui::RegisterBind("bind_ui_mode", "Home", "Toggle gameplay/UI gamepad mode",
                        [this] { pending_keyboard_toggle_ = true; });
}

GamepadUiController::~GamepadUiController() {
  rex::ui::UnregisterBind("bind_ui_mode");
  if (mode_ == Mode::kUi) {
    // Don't leave the guest permanently locked out or ImGui's nav flags
    // permanently on if this controller is torn down mid-UI-mode.
    EnterGameplayMode();
  }
}

void GamepadUiController::ToggleMode() {
  if (mode_ == Mode::kGameplay) {
    EnterUiMode();
  } else {
    EnterGameplayMode();
  }
}

void GamepadUiController::EnterUiMode() {
  mode_ = Mode::kUi;
  if (input_system_) {
    input_system_->SetGuestInputSuppressed(true);
    // Keeps our own GetState() reads live even while the mouse isn't over
    // any overlay window. The normal mouse-capture gate (see rex_app.cpp's
    // SetActiveCallback) would otherwise zero them, same reason the settings
    // overlay's keybind capture uses this.
    input_system_->SetForceActive(true);
  }
  ImGuiIO& io = GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

  ActivateInitialOverlay();

  flash_active_ = true;
  flash_text_ = "UI Mode";
  flash_started_seconds_ = NowSeconds();
  REXLOG_DEBUG("Gamepad UI: entered UI mode");
}

void GamepadUiController::EnterGameplayMode() {
  mode_ = Mode::kGameplay;
  if (input_system_) {
    input_system_->SetGuestInputSuppressed(false);
    input_system_->SetForceActive(false);
  }
  ImGuiIO& io = GetIO();
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

  active_bind_name_.clear();
  active_window_title_.clear();

  flash_active_ = true;
  flash_text_ = "Gameplay Mode";
  flash_started_seconds_ = NowSeconds();
  REXLOG_DEBUG("Gamepad UI: entered Gameplay mode");
}

void GamepadUiController::ActivateInitialOverlay() {
  auto binds = rex::ui::SnapshotBinds();
  for (const auto& bind : binds) {
    if (bind.has_visibility_state && bind.visible && bind.name != "bind_overlay_menu" &&
        bind.name != "bind_overlay_menu_key") {
      SetActiveBind(bind.name);
      return;
    }
  }
  // Nothing else is shown. Fall back to the overlay menu as the hub,
  // opening it if it isn't already.
  for (const auto& bind : binds) {
    if (bind.name == "bind_overlay_menu") {
      if (!bind.visible) {
        rex::ui::InvokeBind("bind_overlay_menu");
      }
      SetActiveBind("bind_overlay_menu");
      return;
    }
  }
}

void GamepadUiController::SetActiveBind(const std::string& bind_name) {
  active_bind_name_ = bind_name;
  active_window_title_.clear();
  for (const auto& bind : rex::ui::SnapshotBinds()) {
    if (bind.name == bind_name) {
      active_window_title_ = bind.window_title;
      break;
    }
  }
  if (!active_window_title_.empty()) {
    ImGui::SetWindowFocus(active_window_title_.c_str());
  }
  REXLOG_DEBUG("Gamepad UI: active overlay -> '{}'", bind_name);
}

void GamepadUiController::OnDraw(ImGuiIO& io) {
  if (pending_keyboard_toggle_) {
    pending_keyboard_toggle_ = false;
    ToggleMode();
  }

  if (mode_ == Mode::kGameplay) {
    // Gameplay mode still needs to poll gamepad-keyed binds (e.g. the
    // overlay menu's own Y trigger, or a mod's gamepad-bound overlay) --
    // this is the one remaining place that does so, now gated to Gameplay
    // only so it doesn't fight this controller's own button handling once
    // UI mode is active (see class comment in the header).
    if (input_system_) {
      rex::input::X_INPUT_STATE state{};
      input_system_->GetState(0, &state);
      const bool guide_down = (state.gamepad.buttons & rex::input::X_INPUT_GAMEPAD_GUIDE) != 0;
      if (guide_down && !guide_was_down_) {
        ToggleMode();
      }
      guide_was_down_ = guide_down;
      if (mode_ == Mode::kGameplay) {  // may have just switched above
        rex::ui::PollGamepadBinds(state);
      }
    }
    DrawModeFlash(io);
    return;
  }

  PollUiNavigation(io);
  DrawModeFlash(io);
}

void GamepadUiController::PollUiNavigation(ImGuiIO& io) {
  if (!input_system_) {
    return;
  }
  rex::input::X_INPUT_STATE state{};
  input_system_->GetState(0, &state);
  const uint16_t buttons = state.gamepad.buttons;

  const bool guide_down = (buttons & rex::input::X_INPUT_GAMEPAD_GUIDE) != 0;
  if (guide_down && !guide_was_down_) {
    guide_was_down_ = guide_down;
    ToggleMode();
    return;  // now in Gameplay mode; nothing else to poll this frame
  }
  guide_was_down_ = guide_down;

  // D-pad + left stick both drive ImGui's item-to-item nav. The stick's
  // deflection is folded into the same dpad keys (past a threshold) rather
  // than the LStick keys, which ImGui only uses for scrolling/tweaking, so
  // the stick moves the cursor exactly like the dpad (key-repeat included).
  const float lx = NormalizeAxis(state.gamepad.thumb_lx);
  const float ly = NormalizeAxis(state.gamepad.thumb_ly);
  constexpr float kNavStickThreshold = 0.5f;
  const bool left =
      (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) != 0 || lx < -kNavStickThreshold;
  const bool right =
      (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) != 0 || lx > kNavStickThreshold;
  const bool up = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) != 0 || ly > kNavStickThreshold;
  const bool down =
      (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) != 0 || ly < -kNavStickThreshold;
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, left);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight, right);
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp, up);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down);

  // A -> confirm/activate the focused widget.
  const bool a_down = (buttons & rex::input::X_INPUT_GAMEPAD_A) != 0;
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, a_down);
  a_was_down_ = a_down;

  // B / Y / X handled directly (edge-triggered), not fed to ImGui nav, so
  // they don't fight it.
  const bool b_down = (buttons & rex::input::X_INPUT_GAMEPAD_B) != 0;
  if (b_down && !b_was_down_) {
    // Close the active overlay, then fall back to another shown overlay or
    // drop back to Gameplay entirely if that was the last one.
    if (!active_bind_name_.empty()) {
      rex::ui::InvokeBind(active_bind_name_);
    }
    bool any_left = false;
    for (const auto& bind : rex::ui::SnapshotBinds()) {
      if (bind.has_visibility_state && bind.visible) {
        SetActiveBind(bind.name);
        any_left = true;
        break;
      }
    }
    if (!any_left) {
      EnterGameplayMode();
    }
  }
  b_was_down_ = b_down;

  const bool y_down = (buttons & rex::input::X_INPUT_GAMEPAD_Y) != 0;
  if (y_down && !y_was_down_) {
    bool menu_visible = false;
    for (const auto& bind : rex::ui::SnapshotBinds()) {
      if (bind.name == "bind_overlay_menu") {
        menu_visible = bind.visible;
        break;
      }
    }
    if (!menu_visible) {
      rex::ui::InvokeBind("bind_overlay_menu");
    }
    SetActiveBind("bind_overlay_menu");
  }
  y_was_down_ = y_down;

  const bool x_down = (buttons & rex::input::X_INPUT_GAMEPAD_X) != 0;
  if (x_down && !x_was_down_) {
    auto binds = rex::ui::SnapshotBinds();
    std::vector<std::string> shown;
    for (const auto& bind : binds) {
      if (bind.has_visibility_state && bind.visible) {
        shown.push_back(bind.name);
      }
    }
    if (!shown.empty()) {
      auto it = std::find(shown.begin(), shown.end(), active_bind_name_);
      size_t next =
          (it == shown.end()) ? 0 : (static_cast<size_t>(it - shown.begin()) + 1) % shown.size();
      SetActiveBind(shown[next]);
    }
  }
  x_was_down_ = x_down;

  // Right stick moves the active overlay; holding left trigger resizes it
  // instead.
  if (!active_window_title_.empty()) {
    const float rx = NormalizeAxis(state.gamepad.thumb_rx);
    const float ry = NormalizeAxis(state.gamepad.thumb_ry);
    if (rx != 0.0f || ry != 0.0f) {
      ImGuiWindow* window = ImGui::FindWindowByName(active_window_title_.c_str());
      if (window) {
        const bool resizing = state.gamepad.left_trigger > kTriggerThreshold;
        if (resizing) {
          ImVec2 size = window->SizeFull;
          size.x = std::max(50.0f, size.x + rx * kWindowResizeSpeed * io.DeltaTime);
          size.y = std::max(50.0f, size.y - ry * kWindowResizeSpeed * io.DeltaTime);
          ImGui::SetWindowSize(window, size);
        } else {
          ImVec2 pos = window->Pos;
          pos.x += rx * kWindowMoveSpeed * io.DeltaTime;
          pos.y -= ry * kWindowMoveSpeed * io.DeltaTime;
          ImGui::SetWindowPos(window, pos);
        }
      }
    }
  }
}

void GamepadUiController::DrawModeFlash(ImGuiIO& io) {
  // UI mode dims the game behind every overlay so it's obvious the pad is no
  // longer driving the guest. The background draw list renders on top of the
  // game but beneath all ImGui windows, so the overlays stay readable.
  const bool ui_mode = mode_ == Mode::kUi;
  if (ui_mode) {
    // Fade the dim in over 0.2s so entering UI mode isn't a hard cut. Reuses
    // the flash timer, which EnterUiMode() (re)starts on every entry.
    double dim_age = NowSeconds() - flash_started_seconds_;
    float dim_alpha = std::clamp(static_cast<float>(dim_age / 0.2), 0.0f, 1.0f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), io.DisplaySize,
        IM_COL32(0, 0, 0, static_cast<int>(128.0f * dim_alpha)));
  }

  if (!flash_active_) {
    return;
  }
  double age = NowSeconds() - flash_started_seconds_;
  // In UI mode the flash stays put the whole time so it's clear the pad only
  // drives the overlays; only the timed Gameplay flash fades out and expires.
  if (!ui_mode && age >= kFlashDisplaySeconds) {
    flash_active_ = false;
    return;
  }

  float alpha = 1.0f;
  if (age < 0.2) {
    alpha = static_cast<float>(age / 0.2);
  } else if (!ui_mode && age > kFlashDisplaySeconds - 0.5) {
    alpha = static_cast<float>((kFlashDisplaySeconds - age) / 0.5);
  }
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.75f * alpha);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
  if (ImGui::Begin("##gamepad_ui_flash", nullptr, flags)) {
    ImGui::TextUnformatted(flash_text_.c_str());
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

}  // namespace rex::ui
