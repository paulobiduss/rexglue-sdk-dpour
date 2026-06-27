/**
 * @file        ui/overlay/pso_compile_indicator_overlay.cpp
 *
 * @brief       Runtime PSO compile indicator overlay. See header for design.
 */
#include <rex/ui/overlay/pso_compile_indicator_overlay.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/graphics/d3d12/pso_compile_indicator_state.h>

REXCVAR_DEFINE_BOOL(show_shader_compile_indicator, false, "UI",
                    "Show a small overlay while PSOs are being compiled in the "
                    "background. Off by default for release — the compile "
                    "queue is silent unless the user opts in. Auto-hides "
                    "~1.2 s after the queue empties.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(shader_compile_indicator_corner, "top_left", "UI",
                      "Where to anchor the compile indicator overlay: "
                      "'top_left', 'top_right', 'bottom_left', 'bottom_right'.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(shader_compile_indicator_verbose, false, "UI",
                    "Add detailed counters (session misses + completions + "
                    "library stores) under the indicator line.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {

// Auto-hide grace after the queue last emptied. Shortened from 2000 ms to
// 1200 ms in v1.0 — the indicator is opt-in now, so the user has chosen to
// see it, but quick-and-quiet beats lingering. The half-second flash is
// still legible.
constexpr std::chrono::milliseconds kIdleGracePeriod{1200};

// Spinner: rotates through dots/braille so the eye sees activity even when
// the counter sits at "1 pending" for a frame.
const char* SpinnerFrame() {
  static const char* kFrames[] = {".  ", ".. ", "...", " ..", "  .", "   "};
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  auto idx = (std::chrono::duration_cast<std::chrono::milliseconds>(now).count() / 120) %
             (sizeof(kFrames) / sizeof(kFrames[0]));
  return kFrames[idx];
}

ImVec2 ChooseAnchor(const ImGuiIO& io, const std::string& corner_value) {
  constexpr float kMargin = 12.0f;
  if (corner_value == "top_right") {
    return ImVec2(io.DisplaySize.x - kMargin, kMargin);
  }
  if (corner_value == "bottom_left") {
    return ImVec2(kMargin, io.DisplaySize.y - kMargin);
  }
  if (corner_value == "bottom_right") {
    return ImVec2(io.DisplaySize.x - kMargin, io.DisplaySize.y - kMargin);
  }
  // top_left (default)
  return ImVec2(kMargin, kMargin);
}

ImVec2 ChoosePivot(const std::string& corner_value) {
  if (corner_value == "top_right") return ImVec2(1.0f, 0.0f);
  if (corner_value == "bottom_left") return ImVec2(0.0f, 1.0f);
  if (corner_value == "bottom_right") return ImVec2(1.0f, 1.0f);
  return ImVec2(0.0f, 0.0f);  // top_left
}

}  // namespace

void PsoCompileIndicatorDialog::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(show_shader_compile_indicator)) {
    return;
  }
  auto& state = rex::graphics::d3d12::PsoCompileIndicatorStateRef();
  const uint32_t busy = state.creation_threads_busy.load(std::memory_order_relaxed);
  const uint32_t queued = state.creation_queue_size.load(std::memory_order_relaxed);
  const uint64_t pending = uint64_t(busy) + uint64_t(queued);

  const auto now = std::chrono::steady_clock::now();
  if (pending > 0) {
    was_ever_active_ = true;
    became_idle_at_ = std::chrono::steady_clock::time_point{};
  } else {
    if (was_ever_active_ && became_idle_at_.time_since_epoch().count() == 0) {
      became_idle_at_ = now;
    }
  }

  // Async-completion backstop: under skip policy, individual async compiles
  // routinely finish between imgui frames so the polling above never observes
  // pending > 0 and the "was_ever_active" flag stays false → overlay never
  // appears. Read the last-completion timestamp written by the creation
  // threads and flash for the same grace window even if we never saw the
  // in-flight state. Encoded as steady_clock nanos since epoch.
  const uint64_t last_completion_ns =
      state.last_completion_steady_ns.load(std::memory_order_relaxed);
  bool recent_async_completion = false;
  if (last_completion_ns != 0) {
    std::chrono::steady_clock::time_point last_completion_tp{
        std::chrono::nanoseconds{last_completion_ns}};
    recent_async_completion =
        (now - last_completion_tp) < kIdleGracePeriod;
  }

  // Sync-mode flash: pso_missing_policy='sync' has no async queue, so the
  // visibility logic above never fires. Check the dedicated sync-compile
  // timestamp and flash for the same grace window after a sync stall.
  const uint64_t last_sync_ns =
      state.last_sync_compile_steady_ns.load(std::memory_order_relaxed);
  bool recent_sync_compile = false;
  std::chrono::milliseconds since_sync{0};
  if (last_sync_ns != 0) {
    std::chrono::steady_clock::time_point last_sync_tp{std::chrono::nanoseconds{last_sync_ns}};
    since_sync = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync_tp);
    recent_sync_compile = since_sync < kIdleGracePeriod;
  }

  // Visible iff pending OR within the grace window since the last async
  // completion (observed-or-unobserved) OR within the grace window since the
  // last sync compile.
  bool visible = pending > 0;
  if (!visible && was_ever_active_) {
    visible = (now - became_idle_at_) < kIdleGracePeriod;
  }
  if (!visible && recent_async_completion) {
    visible = true;
  }
  if (!visible && recent_sync_compile) {
    visible = true;
  }
  if (!visible) {
    return;
  }

  std::string corner = REXCVAR_GET(shader_compile_indicator_corner);
  const auto anchor = ChooseAnchor(io, corner);
  const auto pivot = ChoosePivot(corner);

  ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, pivot);
  // v1.0 polish pass: the indicator is opt-in now, but when it IS on we want
  // it small and quiet — closer to a Steam-style background hint than the
  // previous "emulator notice" footprint. Alpha down to 0.55, fonts halved,
  // border thinner, padding tighter.
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(200, 160, 40, 180));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 20, 200));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
  ImGui::PushFont(nullptr, 14.0f);
  if (ImGui::Begin("##pso_compile_indicator", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (pending > 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.20f, 0.95f), "Shaders%s",
                         SpinnerFrame());
      if (REXCVAR_GET(shader_compile_indicator_verbose)) {
        ImGui::PushFont(nullptr, 11.0f);
        ImGui::Text("%u pending", uint32_t(pending));
        ImGui::PopFont();
      }
    } else if (recent_sync_compile) {
      uint32_t bits = state.last_sync_compile_duration_ms_bits.load(std::memory_order_relaxed);
      float duration_ms = 0.0f;
      std::memcpy(&duration_ms, &bits, sizeof(duration_ms));
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 0.95f),
                         "Shader stall %.0f ms", duration_ms);
    }
    // No "Shaders ready" green flash any more — it added noise without value.
    // Verbose counters block (only if user opted into both indicator + verbose)
    if (pending == 0 && REXCVAR_GET(shader_compile_indicator_verbose)) {
      ImGui::PushFont(nullptr, 10.0f);
      ImGui::Separator();
      const uint64_t total_misses =
          state.total_misses_session.load(std::memory_order_relaxed);
      const uint64_t total_completions =
          state.total_completions_session.load(std::memory_order_relaxed);
      const uint64_t lib_stores =
          state.library_stores_session.load(std::memory_order_relaxed);
      ImGui::Text("misses: %llu  done: %llu  lib: %llu",
                  static_cast<unsigned long long>(total_misses),
                  static_cast<unsigned long long>(total_completions),
                  static_cast<unsigned long long>(lib_stores));
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

}  // namespace rex::ui
