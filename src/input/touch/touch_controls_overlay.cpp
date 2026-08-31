/**
 * @file        input/touch/touch_controls_overlay.cpp
 * @brief       Touch pad overlay. See touch_controls_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Marco Andronaco <andronacomarco@gmail.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/touch/touch_controls_overlay.h>

#include <imgui.h>

#include <algorithm>

#include <rex/cvar.h>

REXCVAR_DECLARE(double, touch_opacity);

namespace rex::input::touch {

namespace {

// Held controls brighten
constexpr float kPressedBoost = 2.0f;

ImU32 FillColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f), 1.0f);
  return ImGui::GetColorU32(ImVec4(0.08f, 0.08f, 0.10f, a));
}

ImU32 EdgeColor(float alpha, bool pressed) {
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.6f, 1.0f);
  return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a));
}

void DrawLabel(ImDrawList* draw_list, const char* label, float cx, float cy, float alpha,
               bool pressed) {
  if (!label || !*label) {
    return;
  }
  const ImVec2 size = ImGui::CalcTextSize(label);
  const float a = std::min(alpha * (pressed ? kPressedBoost : 1.0f) * 1.8f, 1.0f);
  draw_list->AddText(ImVec2(cx - size.x * 0.5f, cy - size.y * 0.5f),
                     ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, a)), label);
}

// One arrowhead of a D-pad, pointing away from the centre along (dx, dy).
void DrawDpadArrow(ImDrawList* draw_list, float cx, float cy, float radius, float dx, float dy,
                   float alpha, bool lit) {
  const float tip = radius * 0.72f;
  const float base = radius * 0.36f;
  const float half = radius * 0.20f;
  // Perpendicular to the direction, for the two base corners.
  const float px = -dy;
  const float py = dx;
  const ImVec2 a(cx + dx * tip, cy + dy * tip);
  const ImVec2 b(cx + dx * base + px * half, cy + dy * base + py * half);
  const ImVec2 c(cx + dx * base - px * half, cy + dy * base - py * half);
  draw_list->AddTriangleFilled(a, b, c, EdgeColor(alpha, lit));
}

}  // namespace

TouchControlsOverlay::TouchControlsOverlay(rex::ui::ImGuiDrawer* drawer, TouchInputDriver* driver)
    : rex::ui::ImGuiDialog(drawer), driver_(driver) {}

void TouchControlsOverlay::OnDraw(ImGuiIO& io) {
  if (!driver_) {
    return;
  }
  TouchVisualState state;
  if (!driver_->GetVisualState(&state)) {
    return;
  }

  const float width = io.DisplaySize.x;
  const float height = io.DisplaySize.y;
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }

  const float alpha = float(std::clamp(REXCVAR_GET(touch_opacity), 0.0, 1.0));
  if (alpha <= 0.0f) {
    return;
  }

  // The driver hit-tested against the window in pixels; ImGui draws in logical
  // points. Rebuilding the layout here rather than scaling the driver's copy
  // keeps both sides reading the same source of truth for the geometry.
  const TouchLayout layout = driver_->BuildLayout(width, height);

  // Foreground list so the pad sits above the game and above any overlay
  // window, drawn without a host window of its own so it never takes input:
  // touches belong to the driver, which sees them straight from the window.
  ImDrawList* draw_list = ImGui::GetForegroundDrawList();

  for (size_t i = 0; i < layout.controls.size(); ++i) {
    const TouchControl& c = layout.controls[i];
    const bool pressed = (state.pressed_mask & (uint64_t(1) << i)) != 0;
    const uint16_t held = i < state.control_buttons.size() ? state.control_buttons[i] : 0;

    switch (c.shape) {
      case TouchControl::Shape::kCircle:
        draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, pressed), 32);
        draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32, 2.0f);
        DrawLabel(draw_list, c.label.c_str(), c.cx, c.cy, alpha, pressed);
        break;

      case TouchControl::Shape::kPill: {
        const ImVec2 lo(c.cx - c.half_width, c.cy - c.half_height);
        const ImVec2 hi(c.cx + c.half_width, c.cy + c.half_height);
        draw_list->AddRectFilled(lo, hi, FillColor(alpha, pressed), c.half_height);
        draw_list->AddRect(lo, hi, EdgeColor(alpha, pressed), c.half_height, 0, 2.0f);
        DrawLabel(draw_list, c.label.c_str(), c.cx, c.cy, alpha, pressed);
        break;
      }

      case TouchControl::Shape::kDpad:
        draw_list->AddCircleFilled(ImVec2(c.cx, c.cy), c.half_width, FillColor(alpha, false), 32);
        draw_list->AddCircle(ImVec2(c.cx, c.cy), c.half_width, EdgeColor(alpha, pressed), 32, 2.0f);
        // Each arrow lights on its own, so a diagonal lights two of them and
        // the drawing matches what the guest is being told.
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, -1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_UP) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 0.0f, 1.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_DOWN) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, -1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_LEFT) != 0);
        DrawDpadArrow(draw_list, c.cx, c.cy, c.half_width, 1.0f, 0.0f, alpha,
                      (held & X_INPUT_GAMEPAD_DPAD_RIGHT) != 0);
        break;
    }
  }

  for (size_t i = 0; i < layout.sticks.size() && i < state.sticks.size(); ++i) {
    const TouchStick& stick = layout.sticks[i];
    const TouchStickVisual& visual = state.sticks[i];
    // A free-placed stick has no resting position, so it only exists on screen
    // while a finger is planting it. A pinned one is always drawn.
    if (!visual.active && !stick.fixed) {
      continue;
    }
    const ImVec2 centre(visual.cx * width, visual.cy * height);
    const ImVec2 knob(visual.knob_cx * width, visual.knob_cy * height);
    draw_list->AddCircleFilled(centre, stick.radius, FillColor(alpha, false), 48);
    draw_list->AddCircle(centre, stick.radius, EdgeColor(alpha, false), 48, 2.0f);
    draw_list->AddCircleFilled(knob, stick.radius * 0.42f, FillColor(alpha, visual.active), 32);
    draw_list->AddCircle(knob, stick.radius * 0.42f, EdgeColor(alpha, visual.active), 32, 2.0f);
  }
}

}  // namespace rex::input::touch
