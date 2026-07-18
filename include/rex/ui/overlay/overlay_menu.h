/**
 * @file        rex/ui/overlay/overlay_menu.h
 * @brief       Gamepad-triggered menu listing every registered overlay
 *              (base app and mod) with its shown/hidden state.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     First real step of the SDK's overlays becoming fully
 *              gamepad-navigable: it is itself opened by a gamepad button
 *              (default Y, rebindable like any other bind -- see
 *              keybinds.h), lists every rex::ui::SnapshotBinds() entry that
 *              exposes visibility state, and selecting a row toggles that
 *              overlay via its own bind callback. Since it's built on the
 *              regular bind registry, both vanilla overlays (debug/console/
 *              settings/mod manager/achievements/shader debugger, once they
 *              opt into passing an is_visible getter to RegisterBind) and
 *              mod overlays that do the same show up with no separate
 *              registration mechanism to keep in sync.
 *
 *              The per-frame gamepad poll (rex::ui::PollGamepadBinds) that
 *              makes gamepad-keyed binds fire at all now lives on
 *              rex::ui::GamepadUiController (see gamepad_ui.h), which gates
 *              it to Gameplay mode -- this dialog no longer polls itself.
 */
#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::ui {

class OverlayMenuDialog : public ImGuiDialog {
 public:
  OverlayMenuDialog(ImGuiDrawer* imgui_drawer, rex::Runtime* runtime);
  ~OverlayMenuDialog() override;

  bool IsVisible() const { return visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;
};

}  // namespace rex::ui
