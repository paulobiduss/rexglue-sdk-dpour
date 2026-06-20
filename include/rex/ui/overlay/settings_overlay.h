/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
  // Save-button state machine. Button click → kConfirmPending opens an
  // explicit ImGui modal so the user has to confirm before the on-disk TOML
  // is rewritten. Earlier behavior wrote on bare click and silently dropped
  // unrecognised TOML entries — see `reference_sdk_saveconfig_wipes_toml.md`.
  bool save_confirm_open_ = false;
  std::string save_status_;
  double save_status_until_ = 0.0;
};

}  // namespace rex::ui
