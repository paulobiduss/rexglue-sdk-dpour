/**
 * @file        rex/graphics/d3d12/pso_compile_indicator_state.h
 *
 * @brief       Lightweight cross-module hook for the runtime PSO compile
 *              indicator overlay. The overlay lives in rexui (does not see
 *              pipeline_cache.h's transitive xxhash dep), so it reads the
 *              live PSO compile state through this thin accessor.
 *
 * Storage + writes live alongside PipelineCache in pipeline_cache.cpp;
 * reads happen from PsoCompileIndicatorDialog::OnDraw.
 *
 * This is read-only data for the overlay. The PipelineCache itself drives
 * the work; we only mirror the counts here for cheap polling from another
 * CMake target.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics::d3d12 {

struct PsoCompileIndicatorState {
  // Pipelines currently being created on a background creation thread.
  std::atomic<uint32_t> creation_threads_busy{0};
  // Pipelines waiting in the priority queue for a free creation thread.
  std::atomic<uint32_t> creation_queue_size{0};
  // Cumulative count of cache misses observed this session — drives the
  // "first time playing this scene" feel of the indicator.
  std::atomic<uint64_t> total_misses_session{0};
  // Cumulative count of PSO creations finished this session (including
  // boot replay). For the verbose readout: ratio between these and
  // total_misses_session ~= what fraction of the session has been new.
  std::atomic<uint64_t> total_completions_session{0};
  // Wall-clock nanoseconds since the epoch when a PSO was last completed.
  // The overlay uses this to "stay visible 2 seconds after last completion"
  // and avoid flickering on sub-frame compiles.
  std::atomic<uint64_t> last_completion_steady_ns{0};
  // Total count of PSOs added to ID3D12PipelineLibrary this session — used
  // for verbose readout "(N new keys this session)".
  std::atomic<uint64_t> library_stores_session{0};
  // ---------------------------------------------------------------------
  // Sync-mode (pso_missing_policy='sync') tracking. With sync policy the
  // render thread blocks on CreateGraphicsPipelineState directly — no
  // async queue, so creation_threads_busy + creation_queue_size stay 0
  // and the overlay would never appear. Bump these atomic counters from
  // the sync branch so the overlay can flash AFTER the stall completes
  // (it cannot draw DURING — render thread is blocked).
  std::atomic<uint64_t> sync_compiles_session{0};
  std::atomic<uint64_t> last_sync_compile_steady_ns{0};
  // Most-recent sync compile duration, in milliseconds. Stored as uint32_t
  // bits to keep the struct trivially-copyable for the cross-module
  // accessor. Encode via std::bit_cast<float, uint32_t> at read time.
  std::atomic<uint32_t> last_sync_compile_duration_ms_bits{0};
};

PsoCompileIndicatorState& PsoCompileIndicatorStateRef();

}  // namespace rex::graphics::d3d12
