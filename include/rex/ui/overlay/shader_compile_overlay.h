/**
 * @file        rex/ui/overlay/shader_compile_overlay.h
 *
 * @brief       Launch-time shader compile progress dialog.
 *
 * Polls GraphicsSystem::shader_storage_progress() each frame and renders a
 * blocking-style modal until the pre-launch persistent shader storage has
 * finished translating + compiling cached pipelines. When the progress
 * `finished` flag flips, fires the on_complete callback exactly once and
 * dismisses itself.
 */
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::graphics {
class GraphicsSystem;
}

namespace rex::ui {

class ShaderCompileDialog final : public ImGuiDialog {
 public:
  using CompleteCallback = std::function<void()>;

  ShaderCompileDialog(ImGuiDrawer* drawer, const rex::graphics::GraphicsSystem* graphics_system,
                      std::string title, std::string subtitle, CompleteCallback complete);

  // Progress-only readout; never accepts clicks or keyboard. Crucially: if the
  // close-via-Close()-then-delete path ever leaks the dialog into dialogs_,
  // returning true here would block MnK forever. False keeps the input system
  // healthy regardless.
  bool WantsInputCapture() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  const rex::graphics::GraphicsSystem* graphics_system_;
  std::string title_;
  std::string subtitle_;
  CompleteCallback complete_;
  bool completion_fired_ = false;
  // Peak pipelines_created observed — clamps the displayed fraction so we
  // never go BACKWARDS if Read() races between total publication and the
  // first increment.
  uint32_t peak_pipelines_created_ = 0;
  // Total reported at any point (latched once observed) so the final summary
  // line keeps showing "Compiled N pipelines" after finished flips and the
  // backing counters get reset for the next init.
  uint32_t peak_pipelines_total_ = 0;
  uint32_t peak_shaders_translated_ = 0;
  std::chrono::steady_clock::time_point opened_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point finished_at_{};
  bool finished_seen_ = false;
  // Hold the dialog visible at least this long even if init completed in
  // microseconds — first-run flash without showing any progress is worse UX
  // than a brief intentional pause that confirms work happened.
  static constexpr std::chrono::milliseconds kMinDisplayDuration{1500};
  // After init completes, hold the "compiled" summary visible for this long
  // so the user gets a beat of "ok, that's done" before the game window
  // appears. Zero would dismiss instantly.
  static constexpr std::chrono::milliseconds kPostFinishedHoldDuration{600};
};

}  // namespace rex::ui
