/**
 * @file        rex/ui/overlay/pso_compile_indicator_overlay.h
 *
 * @brief       Runtime PSO compile indicator overlay.
 *
 * Sibling to the launch-time ShaderCompileDialog: where that one shows the
 * boot/replay progress bar before the game window appears, THIS one is a
 * non-intrusive corner indicator that pops in during gameplay whenever
 * background pipeline compiles are in flight, and auto-hides shortly after
 * the queue empties.
 *
 * The overlay reads its data through the lightweight cross-module
 * accessor PsoCompileIndicatorStateRef() to avoid pulling the heavy
 * pipeline_cache.h transitive deps (xxhash) into rexui.
 */
#pragma once

#include <chrono>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class PsoCompileIndicatorDialog : public ImGuiDialog {
 public:
  explicit PsoCompileIndicatorDialog(ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  // Passive readout; doesn't capture mouse/keyboard.
  bool WantsInputCapture() const override { return false; }
  // Rely on the game's frame pace - we have nothing meaningful to show when
  // the game isn't rendering anyway, and forcing continuous repaint would
  // waste GPU on the indicator just to drive the auto-hide timer. Same
  // policy as the FPS overlay.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // True once we've seen at least one active state - prevents the very first
  // frame (where queue/busy are both zero) from triggering the auto-hide
  // timer immediately.
  bool was_ever_active_ = false;
  std::chrono::steady_clock::time_point became_idle_at_{};
};

}  // namespace rex::ui
