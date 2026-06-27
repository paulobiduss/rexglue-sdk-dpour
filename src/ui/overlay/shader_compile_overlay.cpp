/**
 * @file        ui/overlay/shader_compile_overlay.cpp
 *
 * @brief       Launch-time shader compile progress dialog. See header.
 */
#include <rex/ui/overlay/shader_compile_overlay.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include <imgui.h>

#include <rex/graphics/graphics_system.h>

namespace rex::ui {

ShaderCompileDialog::ShaderCompileDialog(ImGuiDrawer* drawer,
                                         const rex::graphics::GraphicsSystem* graphics_system,
                                         std::string title, std::string subtitle,
                                         CompleteCallback complete)
    : ImGuiDialog(drawer),
      graphics_system_(graphics_system),
      title_(std::move(title)),
      subtitle_(std::move(subtitle)),
      complete_(std::move(complete)) {}

void ShaderCompileDialog::OnDraw(ImGuiIO& io) {
  const auto& progress = graphics_system_->shader_storage_progress();
  const bool finished = progress.finished.load(std::memory_order_acquire);
  const uint32_t shaders = progress.shaders_translated.load(std::memory_order_relaxed);
  const uint32_t pipelines_total_now = progress.pipelines_total.load(std::memory_order_acquire);
  const uint32_t pipelines_created_now =
      progress.pipelines_created.load(std::memory_order_relaxed);
  // Latch peaks — counters are reset on the next InitializeShaderStorage and
  // we want the summary line to stay coherent in the post-finished hold window.
  peak_pipelines_created_ = std::max(peak_pipelines_created_, pipelines_created_now);
  peak_pipelines_total_ = std::max(peak_pipelines_total_, pipelines_total_now);
  peak_shaders_translated_ = std::max(peak_shaders_translated_, shaders);
  const uint32_t pipelines_created = peak_pipelines_created_;
  const uint32_t pipelines_total = peak_pipelines_total_;
  const uint32_t shaders_translated = peak_shaders_translated_;

  if (finished && !finished_seen_) {
    finished_seen_ = true;
    finished_at_ = std::chrono::steady_clock::now();
  }

  const auto now = std::chrono::steady_clock::now();
  const auto elapsed_since_open = now - opened_at_;

  // Compact toast in the top-right corner — short, low padding, no font
  // override. Matches the "Shaders ready" style the user asked for; the
  // launch-time progress is informational, not a modal flow.
  const float toast_width = 320.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(toast_width, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 5.0f));

  if (ImGui::Begin(title_.c_str(), nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav)) {
    char status[160];
    if (finished_seen_) {
      if (pipelines_total > 0) {
        std::snprintf(status, sizeof(status), "Ready: %u PSO / %.1fs",
                      pipelines_created,
                      std::chrono::duration<double>(finished_at_ - opened_at_).count());
      } else {
        std::snprintf(status, sizeof(status), "Cache empty (cold start)");
      }
    } else if (pipelines_total > 0) {
      std::snprintf(status, sizeof(status), "PSO %u / %u", pipelines_created, pipelines_total);
    } else if (shaders_translated > 0) {
      std::snprintf(status, sizeof(status), "Translating shaders %u", shaders_translated);
    } else {
      std::snprintf(status, sizeof(status), "Loading storage...");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(status);
    ImGui::PopTextWrapPos();

    float fraction = 0.0f;
    if (finished_seen_) {
      fraction = 1.0f;
    } else if (pipelines_total > 0) {
      fraction = std::clamp(float(double(pipelines_created) / double(pipelines_total)), 0.0f, 1.0f);
    }
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 6.0f), "");

    ImGui::End();
  }

  ImGui::PopStyleVar(3);

  // Two gates on dismissal: (1) init must be finished, and (2) we've held
  // the dialog visible long enough that the user actually saw what happened.
  // RTX-class hardware can chew through small caches in <100 ms; the brief
  // floor here prevents a "modal flashes and vanishes" mystery.
  if (finished && !completion_fired_) {
    const auto elapsed_since_finished = now - finished_at_;
    const bool min_duration_met = elapsed_since_open >= kMinDisplayDuration;
    const bool post_finished_hold_met = elapsed_since_finished >= kPostFinishedHoldDuration;
    if (min_duration_met && post_finished_hold_met) {
      completion_fired_ = true;
      auto cb = std::move(complete_);
      // Close BEFORE firing so OnPostLaunchModule can rely on the overlay
      // being gone; cb may swap in a different top-level overlay.
      Close();
      if (cb) {
        cb();
      }
      // `this` may have been destroyed by Close()-driven dialog removal;
      // return immediately without touching members.
      return;
    }
  }
}

}  // namespace rex::ui
