/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

// A section a recompiled title can add to the F3 overlay.
//
// The overlay is the SDK's, so it knows nothing about any particular game - but
// a title bringing up a renderer needs its own switches next to the counters
// that show what they did, and rebuilding to flip one is the slowest possible
// way to compare two of them. A section is drawn inside the Debug window, after
// everything the SDK draws, in registration order.
//
// `wants_input` promotes the whole overlay from a passive readout to an
// interactive one while at least one such section is registered: the dialog
// declines input capture otherwise, and a slider nothing can click is not a
// control. Register a read-only section with wants_input = false and the
// overlay stays passive.
using DebugOverlaySectionDrawer = std::function<void()>;

// Returns an id for UnregisterDebugOverlaySection. Safe before the overlay (or
// the UI at all) exists; sections are held statically and drawn if and when it
// opens. Not thread-safe against the draw itself - register during startup.
uint32_t RegisterDebugOverlaySection(std::string_view title, DebugOverlaySectionDrawer drawer,
                                     bool wants_input = true);
void UnregisterDebugOverlaySection(uint32_t id);

// TRUE when any registered section asked for input.
bool DebugOverlayHasInteractiveSection();

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {},
                              std::string_view build_stamp = {});
  ~DebugOverlayDialog();

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

  // Passive readout by default - keyboard toggles are handled at app level, and
  // an overlay that eats input would take it from the game. A title that
  // registers an interactive section is asking for the opposite, so it gets it
  // for as long as that section is registered.
  bool WantsInputCapture() const override { return DebugOverlayHasInteractiveSection(); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameStatsProvider stats_provider_;
  std::string build_stamp_;
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
#endif
};

}  // namespace rex::ui
