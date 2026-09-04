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

#include <rex/assert.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <algorithm>

namespace rex {
namespace ui {

void SetNextWindowFittedSize(const ImGuiIO& io, float width, float height) {
  const ImVec2 limit(io.DisplaySize.x, io.DisplaySize.y);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), limit);
  ImGui::SetNextWindowSize(ImVec2(std::min(width, limit.x), std::min(height, limit.y)),
                           ImGuiCond_FirstUseEver);
}

ImGuiDialog::ImGuiDialog(ImGuiDrawer* imgui_drawer) : imgui_drawer_(imgui_drawer) {
  imgui_drawer_->AddDialog(this);
}

ImGuiDialog::~ImGuiDialog() {
  imgui_drawer_->RemoveDialog(this);
  for (auto fence : waiting_fences_) {
    fence->Signal();
  }
}

void ImGuiDialog::Then(rex::thread::Fence* fence) {
  waiting_fences_.push_back(fence);
}

void ImGuiDialog::Close() {
  has_close_pending_ = true;
}

ImGuiIO& ImGuiDialog::GetIO() {
  return imgui_drawer()->GetIO();
}

void ImGuiDialog::Draw() {
  // Draw UI.
  OnDraw(GetIO());

  // Check to see if the UI closed itself and needs to be deleted.
  if (has_close_pending_) {
    OnClose();
    delete this;
  }
}

class MessageBoxDialog final : public ImGuiDialog {
 public:
  MessageBoxDialog(ImGuiDrawer* imgui_drawer, std::string title, std::string body)
      : ImGuiDialog(imgui_drawer), title_(std::move(title)), body_(std::move(body)) {}

  void OnDraw(ImGuiIO& io) override {
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      char* text = const_cast<char*>(body_.c_str());
      ImGui::InputTextMultiline("##body", text, body_.size(), ImVec2(600, 0),
                                ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
      if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string body_;
};

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer, std::string title,
                                         std::string body) {
  return new MessageBoxDialog(imgui_drawer, std::move(title), std::move(body));
}

}  // namespace ui
}  // namespace rex
