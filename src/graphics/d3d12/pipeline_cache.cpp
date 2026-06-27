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

#include "thirdparty/dxbc/DXBCChecksum.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/filesystem.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/pipeline_cache.h>
#include <rex/graphics/d3d12/pso_compile_indicator_state.h>
#include <rex/perf/guest_cpu_stall_metrics.h>
#include <rex/graphics/d3d12/pso_stall_present_accumulator.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/pipeline_util.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/string/buffer.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <toml++/toml.hpp>

REXCVAR_DEFINE_DOUBLE(d3d12_pso_compile_budget_ms, 0.0, "GPU/D3D12",
                      "Max wall-clock (ms) the render thread is allowed to wait at frame end for "
                      "background pipeline-creation threads to finish queued PSOs. 0 = unlimited "
                      "(current behavior: render thread blocks until ALL queued compiles done, "
                      "causes 100-700 ms spikes on cold-cache transitions). Positive value caps "
                      "the wait: on timeout the frame submits anyway with the still-pending PSOs "
                      "deferred to the next frame's EndSubmission wait. Recommended starting "
                      "values: 8.0 (one 120Hz frame) or 16.0 (one 60Hz frame). May cause brief "
                      "missing-geometry visual artifacts while compiles roll over, but converts "
                      "single CRITICAL 370ms spike into 2-3 MILD 80-120ms hitches. See also "
                      "d3d12_pso_no_block_at_submission_end which short-circuits this wait "
                      "entirely.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_pso_no_block_at_submission_end, true, "GPU/D3D12",
                    "When true (default), PipelineCache::EndSubmission SKIPS the render-thread "
                    "wait on in-flight async PSO compiles entirely — the frame submits "
                    "immediately and pending PSOs roll forward to be picked up on later frames. "
                    "Trade: brief 'skipped-draws' visual artifact on first encounter with a new "
                    "PSO permutation, no render-thread stall. When false, the legacy behavior "
                    "applies: render thread waits up to d3d12_pso_compile_budget_ms at frame end, "
                    "which produces per-frame milliseconds of stutter even with "
                    "pso_missing_policy='skip'. Leave true unless you can't tolerate any pop-in "
                    "and prefer stutters in exchange for guaranteed-complete geometry.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(pso_missing_policy, "skip", "GPU",
                      "Behavior when a draw call's PSO is still being compiled on a background "
                      "thread: 'skip' (default, current behavior — draw is dropped, geometry "
                      "invisible this frame), 'block' (render thread spin-waits on the PSO state "
                      "until non-null, geometry never lost but stalls the frame), 'sync' (no "
                      "async compile at all, every miss compiles inline on the render thread)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(d3d12_pso_block_per_draw_budget_ms, 8.0, "GPU/D3D12",
                      "Per-draw wait budget (ms) when pso_missing_policy='block'. The render "
                      "thread spin-yields up to this many ms waiting for a single draw's PSO to "
                      "finish compiling; on timeout the draw is skipped (single sub-frame "
                      "missing geometry, not a multi-frame freeze). Default 8.0 = half a 60Hz "
                      "frame, so worst-case freeze stays invisible while letting fast compiles "
                      "complete in-line. Set to 500.0 to restore the legacy 'block forever' "
                      "behavior up to the hard safety valve.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pso_stall_log_verbose, false, "GPU",
                    "When true, emit PSO_STALL[compile-only] WARN lines whenever any secondary "
                    "metric (submit_ms / present_ms / tex_req_ms / rt_resolve_ms / "
                    "submit_barriers_ms / fence_wait_ms) crosses its 5 ms diagnostic threshold. "
                    "These thresholds fire on every healthy GPU-bound frame (fence_wait > 5 ms is "
                    "normal at 60 FPS), so default-on flooded the log with no actionable signal. "
                    "Enable for deep stutter investigation; primary triggers (frame spike > 25 ms, "
                    "PSO misses, draw skips, block_wait) keep logging regardless.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_dxbc_disasm, false, "GPU/D3D12", "Dump DXBC disassembly");

REXCVAR_DEFINE_BOOL(d3d12_dxbc_disasm_dxilconv, false, "GPU/D3D12",
                    "Dump DXIL conversion disassembly");

REXCVAR_DEFINE_INT32(d3d12_pipeline_creation_threads, -1, "GPU/D3D12",
                     "Number of pipeline creation threads (-1 for auto)")
    .range(-1, 32)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_pso_library_enable, true, "GPU/D3D12",
                    "Cache compiled D3D12 PSO blobs across runs in an "
                    "ID3D12PipelineLibrary file ('*.pso_lib' next to '.xpso'). "
                    "On warm boot, LoadGraphicsPipeline hits skip the 100-800 ms "
                    "driver compile per PSO and return an existing state in "
                    "microseconds. The library is GPU/driver-specific and is "
                    "automatically invalidated by D3D12 on driver/adapter change "
                    "(blob is rebuilt from scratch).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(pso_capture, false, "GPU",
                    "When true, every runtime-miss PSO is logged at WARN with "
                    "its full description hash + shader hashes (capture mode "
                    "for building a portable .xpso for shipping). The .xpso "
                    "file is written regardless; this cvar only adds verbose "
                    "per-miss logging.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(pso_capture_dump_on_exit, false, "GPU",
                    "When true, ShutdownShaderStorage writes a parallel "
                    "*.xpso.capture.toml file listing every Pipeline observed "
                    "this session (hash + origin + usage_count + first/last "
                    "frame). Useful when bundling a shareable database for a "
                    "release.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_tessellation_wireframe, false, "GPU/D3D12",
                    "Render tessellation as wireframe");

namespace rex::graphics::d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "../shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "../shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "../shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "../shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
}  // namespace shaders

PipelineCache::PipelineCache(D3D12CommandProcessor& command_processor,
                             const RegisterFile& register_file,
                             const D3D12RenderTargetCache& render_target_cache,
                             bool bindless_resources_used)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      bindless_resources_used_(bindless_resources_used) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();

  bool edram_rov_used =
      render_target_cache.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !render_target_cache_.gamma_render_target_as_unorm16(),
      render_target_cache_.msaa_2x_supported(), render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(), provider.GetGraphicsAnalysis() != nullptr);

  if (edram_rov_used) {
    depth_only_pixel_shader_ = std::move(shader_translator_->CreateDepthOnlyPixelShader());
  }
}

PipelineCache::~PipelineCache() {
  Shutdown();
}

bool PipelineCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();

  // Initialize the command processor thread DXIL objects.
  dxbc_converter_ = nullptr;
  dxc_utils_ = nullptr;
  dxc_compiler_ = nullptr;
  if (REXCVAR_GET(d3d12_dxbc_disasm_dxilconv)) {
    if (FAILED(provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                                    IID_PPV_ARGS(&dxbc_converter_)))) {
      REXGPU_ERROR(
          "Failed to create DxbcConverter, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils_)))) {
      REXGPU_ERROR(
          "Failed to create DxcUtils, converted DXIL disassembly for debugging "
          "will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler_)))) {
      REXGPU_ERROR(
          "Failed to create DxcCompiler, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
  }

  uint32_t logical_processor_count = rex::thread::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  // Initialize creation thread synchronization data even if not using creation
  // threads because they may be used anyway to create pipelines from the
  // storage.
  creation_threads_busy_ = 0;
  creation_completion_event_ = rex::thread::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (REXCVAR_GET(d3d12_pipeline_creation_threads) != 0) {
    size_t creation_thread_count;
    if (REXCVAR_GET(d3d12_pipeline_creation_threads) < 0) {
      creation_thread_count = std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(REXCVAR_GET(d3d12_pipeline_creation_threads)), logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<rex::thread::Thread> creation_thread =
          rex::thread::Thread::Create({}, [this, i]() { CreationThread(i); });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void PipelineCache::Shutdown() {
  // Shut down all threads, before destroying the pipelines since they may be
  // creating them.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      rex::thread::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  creation_completion_event_.reset();

  // Shut down the persistent shader / pipeline storage.
  ShutdownShaderStorage();

  // Destroy all pipelines.
  current_pipeline_ = nullptr;
  for (auto it : pipelines_) {
    ID3D12PipelineState* state = it.second->state.load(std::memory_order_acquire);
    if (state) {
      state->Release();
    }
    delete it.second;
  }
  pipelines_.clear();
  COUNT_profile_set("gpu/pipeline_cache/pipelines", 0);

  // Destroy all shaders.
  if (bindless_resources_used_) {
    bindless_sampler_layout_map_.clear();
    bindless_sampler_layouts_.clear();
  }
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  shader_storage_index_ = 0;

  // Shut down shader translation.
  ui::d3d12::util::ReleaseAndNull(dxc_compiler_);
  ui::d3d12::util::ReleaseAndNull(dxc_utils_);
  ui::d3d12::util::ReleaseAndNull(dxbc_converter_);
}

void PipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                            uint32_t title_id, bool blocking) {
  ShutdownShaderStorage();

  // Progress reporting for the launch-time compile overlay. Reset to zero,
  // mark in-progress; the dialog is already running on the UI thread and
  // polling these atomics while we work here on the CP thread.
  auto& progress = command_processor_.graphics_system()->shader_storage_progress();
  progress.shaders_translated.store(0, std::memory_order_relaxed);
  progress.pipelines_created.store(0, std::memory_order_relaxed);
  progress.pipelines_total.store(0, std::memory_order_relaxed);
  progress.finished.store(false, std::memory_order_relaxed);
  progress.in_progress.store(true, std::memory_order_release);

  auto shader_storage_root = cache_root / "shaders";
  // For files that can be moved between different hosts.
  // Host PSO blobs - if ever added - should be stored in shaders/local/ (they
  // currently aren't used because because they may be not very practical -
  // would need to invalidate them every commit likely, and additional I/O
  // cost - though D3D's internal validation would possibly be enough to ensure
  // they are up to date).
  auto shader_storage_shareable_root = shader_storage_root / "shareable";
  if (!std::filesystem::exists(shader_storage_shareable_root)) {
    if (!std::filesystem::create_directories(shader_storage_shareable_root)) {
      REXGPU_ERROR(
          "Failed to create the shareable shader storage directory, persistent "
          "shader storage will be disabled: {}",
          rex::path_to_utf8(shader_storage_shareable_root));
      return;
    }
  }

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  const uint32_t draw_resolution_scale_x = render_target_cache_.draw_resolution_scale_x();
  const uint32_t draw_resolution_scale_y = render_target_cache_.draw_resolution_scale_y();
  const std::string shader_storage_scale_key =
      fmt::format("scale{}x{}", draw_resolution_scale_x, draw_resolution_scale_y);
  REXGPU_INFO("Using D3D12 shader storage key {} for title {:08X}", shader_storage_scale_key,
              title_id);

  // Initialize the pipeline storage stream - read pipeline descriptions and
  // collect used shader modifications to translate.
  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  // <Shader hash, modification bits>.
  std::set<std::pair<uint64_t, uint64_t>> shader_translations_needed;
  // Cache key versioning (docx §37, mandatory): include the translator and
  // PSO-description versions in the FILENAME so a translator semantic change
  // produces a fresh cache file instead of silently reusing a stale one. The
  // file header still carries the magic + version check (line ~308) for the
  // same reason. Old caches are left on disk so a downgrade can pick them
  // back up if needed; we never automatically delete a non-matching cache.
  const uint32_t shader_cache_key_version =
      std::max(PipelineDescription::kVersion, DxbcShaderTranslator::Modification::kVersion);
  auto pipeline_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.{}.v{:08X}.d3d12.xpso", title_id, shader_storage_scale_key,
                  edram_rov_used ? "rov" : "rtv", shader_cache_key_version);
  auto shader_storage_file_path_seed =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.v{:08X}.xsh", title_id, shader_storage_scale_key,
                  shader_cache_key_version);
  auto pipeline_metadata_sidecar_path_seed = pipeline_storage_file_path;
  pipeline_metadata_sidecar_path_seed += ".meta.toml";

  // Seed-cache import: if no user .xpso / .xsh / sidecar for this title +
  // scale + render path yet, copy a pre-built bundle from
  // <exe_dir>/cache/shaders/shareable/. The shipped seed is what unblocks
  // first-launch PSO stutter for new users — see project_SHIPPING_CONFIG.md
  // (Phase 1 priority). Filenames must match between seed and user paths
  // (same title_id + scale + render path + version), otherwise the seed is
  // simply ignored. Copy is idempotent: existing user files are never
  // overwritten.
  {
    std::error_code ec;
    auto seed_root = rex::filesystem::GetExecutableFolder() / "cache" / "shaders" / "shareable";
    if (std::filesystem::is_directory(seed_root, ec)) {
      auto try_seed = [&](const std::filesystem::path& dest) -> bool {
        if (std::filesystem::exists(dest, ec)) return false;
        auto src = seed_root / dest.filename();
        if (!std::filesystem::is_regular_file(src, ec)) return false;
        std::filesystem::copy_file(src, dest, ec);
        if (ec) {
          REXGPU_INFO("PSO_DB: seed copy failed for '{}': {}",
                      rex::path_to_utf8(dest.filename()), ec.message());
          return false;
        }
        return true;
      };
      const bool seeded_xpso = try_seed(pipeline_storage_file_path);
      const bool seeded_xsh = try_seed(shader_storage_file_path_seed);
      const bool seeded_meta = try_seed(pipeline_metadata_sidecar_path_seed);
      if (seeded_xpso || seeded_xsh || seeded_meta) {
        REXGPU_WARN(
            "PSO_DB: seeded user cache from <exe>/cache/shaders/shareable "
            "(xpso={} xsh={} meta={}); this should be a one-time event on "
            "first launch.",
            seeded_xpso, seeded_xsh, seeded_meta);
      }
    }
  }

  // Portable shareable database metadata sidecar (Phase 1). Parallel to the
  // .xpso binary; written at shutdown, validated at init. Mismatch = WARN,
  // not silent. Stored as TOML for consistency with the rest of the SDK
  // config files (cf. simple_settings_overlay.cpp). Per-entry binary hashes
  // in .xpso still gate the actual data, so a mismatch is informational
  // rather than destructive - we still attempt to load.
  pipeline_metadata_sidecar_path_ = pipeline_storage_file_path;
  pipeline_metadata_sidecar_path_ += ".meta.toml";
  sidecar_metadata_mismatch_ = false;
  session_portable_keys_loaded_.store(0, std::memory_order_relaxed);
  session_prewarmed_pipelines_.store(0, std::memory_order_relaxed);
  session_runtime_misses_.store(0, std::memory_order_relaxed);
  session_spikes_over_16ms_.store(0, std::memory_order_relaxed);
  session_spikes_over_100ms_.store(0, std::memory_order_relaxed);
  session_new_keys_captured_.store(0, std::memory_order_relaxed);
  session_frame_counter_.store(0, std::memory_order_relaxed);
  {
    std::error_code ec;
    if (std::filesystem::is_regular_file(pipeline_metadata_sidecar_path_, ec)) {
      try {
        auto sidecar = toml::parse_file(pipeline_metadata_sidecar_path_.string());
        const uint32_t got_title_id =
            uint32_t(sidecar["title_id"].value_or(int64_t(0)));
        const uint32_t got_desc_ver =
            uint32_t(sidecar["description_version"].value_or(int64_t(0)));
        const uint32_t got_mod_ver =
            uint32_t(sidecar["modification_version"].value_or(int64_t(0)));
        const std::string got_render_path =
            sidecar["render_path"].value_or<std::string>("");
        const int64_t got_scale_x = sidecar["scale_x"].value_or(int64_t(0));
        const int64_t got_scale_y = sidecar["scale_y"].value_or(int64_t(0));
        const std::string want_render_path = edram_rov_used ? "rov" : "rtv";
        if (got_title_id != title_id || got_desc_ver != PipelineDescription::kVersion ||
            got_mod_ver != DxbcShaderTranslator::Modification::kVersion ||
            got_render_path != want_render_path ||
            got_scale_x != int64_t(draw_resolution_scale_x) ||
            got_scale_y != int64_t(draw_resolution_scale_y)) {
          REXGPU_WARN(
              "PSO_DB: sidecar metadata mismatch at '{}' "
              "(got title=0x{:08X} desc_ver=0x{:08X} mod_ver=0x{:08X} path={} {}x{}; "
              "want title=0x{:08X} desc_ver=0x{:08X} mod_ver=0x{:08X} path={} {}x{}). "
              "Per-entry hashes in the .xpso file will still gate the actual data; "
              "this warning is informational - rebuild the sidecar by clean-exiting "
              "this session.",
              rex::path_to_utf8(pipeline_metadata_sidecar_path_), got_title_id, got_desc_ver,
              got_mod_ver, got_render_path, got_scale_x, got_scale_y, title_id,
              PipelineDescription::kVersion, DxbcShaderTranslator::Modification::kVersion,
              want_render_path, draw_resolution_scale_x, draw_resolution_scale_y);
          sidecar_metadata_mismatch_ = true;
        } else {
          REXGPU_INFO("PSO_DB: sidecar metadata OK at '{}'",
                      rex::path_to_utf8(pipeline_metadata_sidecar_path_));
        }
      } catch (const toml::parse_error& e) {
        REXGPU_WARN("PSO_DB: sidecar parse failure at '{}': {}",
                    rex::path_to_utf8(pipeline_metadata_sidecar_path_), e.description());
        sidecar_metadata_mismatch_ = true;
      }
    }
  }

  // Initialize ID3D12PipelineLibrary blob cache. Stored next to .xpso under
  // the same versioning so a SDK bump on PSO description or shader
  // modification invalidates both. The library is GPU-/driver-specific; on a
  // driver mismatch CreatePipelineLibrary returns
  // D3D12_ERROR_DRIVER_VERSION_MISMATCH or D3D12_ERROR_ADAPTER_NOT_FOUND, and
  // we transparently start from an empty library. The blob memory must remain
  // alive for the lifetime of the library, so pipeline_library_blob_ keeps
  // the read buffer.
  pipeline_library_.Reset();
  pipeline_library_blob_.clear();
  pipeline_library_path_.clear();
  pipeline_library_load_hits_.store(0, std::memory_order_relaxed);
  pipeline_library_store_count_.store(0, std::memory_order_relaxed);
  pipeline_library_store_failures_.store(0, std::memory_order_relaxed);
  pipeline_library_dirty_.store(false, std::memory_order_relaxed);
  if (REXCVAR_GET(d3d12_pso_library_enable)) {
    ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
    Microsoft::WRL::ComPtr<ID3D12Device1> device1;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
      // .pso_lib lives in cache/shaders/local/ so the portable shareable
      // dir (.xpso + .xsh + .xpso.meta.toml) is unambiguously "safe to
      // bundle in a release", while the GPU/driver-specific blob is
      // unambiguously "rebuilt locally per machine". This matches the
      // user's Phase-1 spec for the bundled vs local split.
      auto shader_storage_local_root = shader_storage_root / "local";
      std::error_code ec;
      std::filesystem::create_directories(shader_storage_local_root, ec);
      pipeline_library_path_ =
          shader_storage_local_root /
          fmt::format("{:08X}.{}.{}.v{:08X}.d3d12.pso_lib", title_id, shader_storage_scale_key,
                      edram_rov_used ? "rov" : "rtv", shader_cache_key_version);
      // One-time migration: if a legacy blob exists in the old shareable/
      // location (from 0.8.2.14 + Phase 1 builds), move it into local/ so
      // existing users keep their warm-boot speedup transparently.
      auto legacy_path =
          shader_storage_shareable_root /
          fmt::format("{:08X}.{}.{}.v{:08X}.d3d12.pso_lib", title_id, shader_storage_scale_key,
                      edram_rov_used ? "rov" : "rtv", shader_cache_key_version);
      if (std::filesystem::is_regular_file(legacy_path, ec) &&
          !std::filesystem::is_regular_file(pipeline_library_path_, ec)) {
        std::filesystem::rename(legacy_path, pipeline_library_path_, ec);
        if (!ec) {
          REXGPU_INFO("PSO library migrated to local/ from legacy shareable/ location");
        }
      }
      FILE* lib_file = rex::filesystem::OpenFile(pipeline_library_path_, "rb");
      if (lib_file) {
        rex::filesystem::Seek(lib_file, 0, SEEK_END);
        int64_t size = rex::filesystem::Tell(lib_file);
        if (size > 0 && size < int64_t(1) << 30) {
          pipeline_library_blob_.resize(size_t(size));
          rex::filesystem::Seek(lib_file, 0, SEEK_SET);
          if (fread(pipeline_library_blob_.data(), 1, pipeline_library_blob_.size(), lib_file) !=
              pipeline_library_blob_.size()) {
            pipeline_library_blob_.clear();
          }
        }
        fclose(lib_file);
      }
      HRESULT hr = device1->CreatePipelineLibrary(
          pipeline_library_blob_.empty() ? nullptr : pipeline_library_blob_.data(),
          pipeline_library_blob_.size(), IID_PPV_ARGS(&pipeline_library_));
      if (FAILED(hr)) {
        // E_INVALIDARG, D3D12_ERROR_DRIVER_VERSION_MISMATCH, or
        // D3D12_ERROR_ADAPTER_NOT_FOUND - start over with an empty library.
        REXGPU_INFO(
            "PSO library blob unusable (HRESULT 0x{:08X}), rebuilding from scratch", uint32_t(hr));
        pipeline_library_blob_.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library_));
        if (FAILED(hr)) {
          REXGPU_INFO("PSO library disabled (CreatePipelineLibrary failed 0x{:08X})", uint32_t(hr));
          pipeline_library_.Reset();
        } else {
          pipeline_library_dirty_.store(true, std::memory_order_relaxed);
        }
      } else {
        REXGPU_WARN("PSO library blob loaded ({} bytes from '{}')", pipeline_library_blob_.size(),
                    rex::path_to_utf8(pipeline_library_path_));
      }
    }
  }

  pipeline_storage_file_ = rex::filesystem::OpenFile(pipeline_storage_file_path, "a+b");
  if (!pipeline_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the Direct3D 12 pipeline description storage file for "
        "writing, persistent shader storage will be disabled: {}",
        rex::path_to_utf8(pipeline_storage_file_path));
    return;
  }
  pipeline_storage_file_flush_needed_ = false;
  // 'XEPS'.
  const uint32_t pipeline_storage_magic = 0x53504558;
  // 'DXRO' or 'DXRT'.
  const uint32_t pipeline_storage_magic_api = edram_rov_used ? 0x4F525844 : 0x54525844;
  const uint32_t pipeline_storage_version_swapped = rex::byte_swap(
      std::max(PipelineDescription::kVersion, DxbcShaderTranslator::Modification::kVersion));
  struct {
    uint32_t magic;
    uint32_t magic_api;
    uint32_t version_swapped;
  } pipeline_storage_file_header;
  if (fread(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
            pipeline_storage_file_) &&
      pipeline_storage_file_header.magic == pipeline_storage_magic &&
      pipeline_storage_file_header.magic_api == pipeline_storage_magic_api &&
      pipeline_storage_file_header.version_swapped == pipeline_storage_version_swapped) {
    rex::filesystem::Seek(pipeline_storage_file_, 0, SEEK_END);
    int64_t pipeline_storage_told_end = rex::filesystem::Tell(pipeline_storage_file_);
    size_t pipeline_storage_told_count =
        size_t(pipeline_storage_told_end >= int64_t(sizeof(pipeline_storage_file_header))
                   ? (uint64_t(pipeline_storage_told_end) - sizeof(pipeline_storage_file_header)) /
                         sizeof(PipelineStoredDescription)
                   : 0);
    if (pipeline_storage_told_count &&
        rex::filesystem::Seek(pipeline_storage_file_, int64_t(sizeof(pipeline_storage_file_header)),
                              SEEK_SET)) {
      pipeline_stored_descriptions.resize(pipeline_storage_told_count);
      pipeline_stored_descriptions.resize(
          fread(pipeline_stored_descriptions.data(), sizeof(PipelineStoredDescription),
                pipeline_storage_told_count, pipeline_storage_file_));
      size_t pipeline_storage_read_count = pipeline_stored_descriptions.size();
      session_portable_keys_loaded_.store(uint64_t(pipeline_storage_read_count),
                                          std::memory_order_relaxed);
      REXGPU_WARN("PSO_DB: loaded {} portable keys from '{}'", pipeline_storage_read_count,
                  rex::path_to_utf8(pipeline_storage_file_path));
      // Publish the upper-bound count to the launch overlay so the progress
      // bar has a denominator before the first pipeline starts compiling.
      progress.pipelines_total.store(uint32_t(pipeline_storage_read_count),
                                     std::memory_order_release);
      for (size_t i = 0; i < pipeline_storage_read_count; ++i) {
        const PipelineStoredDescription& pipeline_stored_description =
            pipeline_stored_descriptions[i];
        // Validate file integrity, stop and truncate the stream if data is
        // corrupted.
        if (XXH3_64bits(&pipeline_stored_description.description,
                        sizeof(pipeline_stored_description.description)) !=
            pipeline_stored_description.description_hash) {
          pipeline_stored_descriptions.resize(i);
          break;
        }
        // TODO(Triang3l): On Vulkan, skip pipelines requiring unsupported
        // device features (to keep the cache files mostly shareable across
        // devices).
        // Mark the shader modifications as needed for translation.
        shader_translations_needed.emplace(
            pipeline_stored_description.description.vertex_shader_hash,
            pipeline_stored_description.description.vertex_shader_modification);
        if (pipeline_stored_description.description.pixel_shader_hash) {
          shader_translations_needed.emplace(
              pipeline_stored_description.description.pixel_shader_hash,
              pipeline_stored_description.description.pixel_shader_modification);
        }
      }
    }
  }

  size_t logical_processor_count = rex::thread::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }

  // Initialize the Xenos shader storage stream.
  uint64_t shader_storage_initialization_start = rex::chrono::Clock::QueryHostTickCount();
  // Same versioning policy as the .xpso file above — keep filenames in sync.
  auto shader_storage_file_path =
      shader_storage_shareable_root /
      fmt::format("{:08X}.{}.v{:08X}.xsh", title_id, shader_storage_scale_key,
                  shader_cache_key_version);
  shader_storage_file_ = rex::filesystem::OpenFile(shader_storage_file_path, "a+b");
  if (!shader_storage_file_) {
    REXGPU_ERROR(
        "Failed to open the guest shader storage file for writing, persistent "
        "shader storage will be disabled: {}",
        rex::path_to_utf8(shader_storage_file_path));
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    return;
  }
  ++shader_storage_index_;
  shader_storage_file_flush_needed_ = false;
  struct {
    uint32_t magic;
    uint32_t version_swapped;
  } shader_storage_file_header;
  // 'XESH'.
  const uint32_t shader_storage_magic = 0x48534558;
  if (fread(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
            shader_storage_file_) &&
      shader_storage_file_header.magic == shader_storage_magic &&
      rex::byte_swap(shader_storage_file_header.version_swapped) == ShaderStoredHeader::kVersion) {
    uint64_t shader_storage_valid_bytes = sizeof(shader_storage_file_header);
    // Load and translate shaders written by previous Xenia executions until the
    // end of the file or until a corrupted one is detected.
    ShaderStoredHeader shader_header;
    std::vector<uint32_t> ucode_dwords;
    ucode_dwords.reserve(0xFFFF);
    size_t shaders_translated = 0;

    // Threads overlapping file reading.
    std::mutex shaders_translation_thread_mutex;
    std::condition_variable shaders_translation_thread_cond;
    std::deque<D3D12Shader*> shaders_to_translate;
    size_t shader_translation_threads_busy = 0;
    bool shader_translation_threads_shutdown = false;
    std::mutex shaders_failed_to_translate_mutex;
    std::vector<D3D12Shader::D3D12Translation*> shaders_failed_to_translate;
    auto shader_translation_thread_function = [&]() {
      const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
      string::StringBuffer ucode_disasm_buffer;
      DxbcShaderTranslator translator(
          provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
          !render_target_cache_.gamma_render_target_as_unorm16(),
          render_target_cache_.msaa_2x_supported(), render_target_cache_.draw_resolution_scale_x(),
          render_target_cache_.draw_resolution_scale_y(),
          provider.GetGraphicsAnalysis() != nullptr);
      // If needed and possible, create objects needed for DXIL conversion and
      // disassembly on this thread.
      IDxbcConverter* dxbc_converter = nullptr;
      IDxcUtils* dxc_utils = nullptr;
      IDxcCompiler* dxc_compiler = nullptr;
      if (REXCVAR_GET(d3d12_dxbc_disasm_dxilconv) && dxbc_converter_ && dxc_utils_ &&
          dxc_compiler_) {
        provider.DxbcConverterCreateInstance(CLSID_DxbcConverter, IID_PPV_ARGS(&dxbc_converter));
        provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
        provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
      }
      for (;;) {
        D3D12Shader* shader_to_translate;
        for (;;) {
          std::unique_lock<std::mutex> lock(shaders_translation_thread_mutex);
          if (shaders_to_translate.empty()) {
            if (shader_translation_threads_shutdown) {
              return;
            }
            shaders_translation_thread_cond.wait(lock);
            continue;
          }
          shader_to_translate = shaders_to_translate.front();
          shaders_to_translate.pop_front();
          ++shader_translation_threads_busy;
          break;
        }
        shader_to_translate->AnalyzeUcode(ucode_disasm_buffer);
        // Translate each needed modification on this thread after performing
        // modification-independent analysis of the whole shader.
        uint64_t ucode_data_hash = shader_to_translate->ucode_data_hash();
        for (auto modification_it = shader_translations_needed.lower_bound(
                 std::make_pair(ucode_data_hash, uint64_t(0)));
             modification_it != shader_translations_needed.end() &&
             modification_it->first == ucode_data_hash;
             ++modification_it) {
          D3D12Shader::D3D12Translation* translation = static_cast<D3D12Shader::D3D12Translation*>(
              shader_to_translate->GetOrCreateTranslation(modification_it->second));
          // Only try (and delete in case of failure) if it's a new translation.
          // If it's a shader previously encountered in the game, translation of
          // which has failed, and the shader storage is loaded later, keep it
          // this way not to try to translate it again.
          if (!translation->is_translated() &&
              !TranslateAnalyzedShader(translator, *translation, dxbc_converter, dxc_utils,
                                       dxc_compiler)) {
            std::lock_guard<std::mutex> lock(shaders_failed_to_translate_mutex);
            shaders_failed_to_translate.push_back(translation);
          }
        }
        {
          std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
          --shader_translation_threads_busy;
        }
      }
      if (dxc_compiler) {
        dxc_compiler->Release();
      }
      if (dxc_utils) {
        dxc_utils->Release();
      }
      if (dxbc_converter) {
        dxbc_converter->Release();
      }
    };
    std::vector<std::unique_ptr<rex::thread::Thread>> shader_translation_threads;

    while (true) {
      if (!fread(&shader_header, sizeof(shader_header), 1, shader_storage_file_)) {
        break;
      }
      size_t ucode_byte_count = shader_header.ucode_dword_count * sizeof(uint32_t);
      ucode_dwords.resize(shader_header.ucode_dword_count);
      if (shader_header.ucode_dword_count &&
          !fread(ucode_dwords.data(), ucode_byte_count, 1, shader_storage_file_)) {
        break;
      }
      uint64_t ucode_data_hash = XXH3_64bits(ucode_dwords.data(), ucode_byte_count);
      if (shader_header.ucode_data_hash != ucode_data_hash) {
        // Validation failed.
        break;
      }
      shader_storage_valid_bytes += sizeof(shader_header) + ucode_byte_count;
      D3D12Shader* shader = LoadShader(shader_header.type, ucode_dwords.data(),
                                       shader_header.ucode_dword_count, ucode_data_hash);
      if (shader->ucode_storage_index() == shader_storage_index_) {
        // Appeared twice in this file for some reason - skip, otherwise race
        // condition will be caused by translating twice in parallel.
        continue;
      }
      // Loaded from the current storage - don't write again.
      shader->set_ucode_storage_index(shader_storage_index_);
      // Create new threads if the currently existing threads can't keep up
      // with file reading, but not more than the number of logical processors
      // minus one.
      size_t shader_translation_threads_needed;
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_needed =
            std::min(shader_translation_threads_busy + shaders_to_translate.size() + size_t(1),
                     logical_processor_count - size_t(1));
      }
      while (shader_translation_threads.size() < shader_translation_threads_needed) {
        auto thread = rex::thread::Thread::Create({}, shader_translation_thread_function);
        assert_not_null(thread);
        thread->set_name("Shader Translation");
        shader_translation_threads.push_back(std::move(thread));
      }
      // Request ucode information gathering and translation of all the needed
      // shaders.
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shaders_to_translate.push_back(shader);
      }
      shaders_translation_thread_cond.notify_one();
      ++shaders_translated;
      progress.shaders_translated.fetch_add(1, std::memory_order_relaxed);
    }
    if (!shader_translation_threads.empty()) {
      {
        std::lock_guard<std::mutex> lock(shaders_translation_thread_mutex);
        shader_translation_threads_shutdown = true;
      }
      shaders_translation_thread_cond.notify_all();
      for (auto& shader_translation_thread : shader_translation_threads) {
        rex::thread::Wait(shader_translation_thread.get(), false);
      }
      shader_translation_threads.clear();
      for (D3D12Shader::D3D12Translation* translation : shaders_failed_to_translate) {
        D3D12Shader* shader = static_cast<D3D12Shader*>(&translation->shader());
        shader->DestroyTranslation(translation->modification());
        if (shader->translations().empty()) {
          shaders_.erase(shader->ucode_data_hash());
          delete shader;
        }
      }
    }
    REXGPU_INFO("Translated {} shaders from the storage in {} milliseconds", shaders_translated,
                (rex::chrono::Clock::QueryHostTickCount() - shader_storage_initialization_start) *
                    1000 / rex::chrono::Clock::QueryHostTickFrequency());
    rex::filesystem::TruncateStdioFile(shader_storage_file_, shader_storage_valid_bytes);
  } else {
    rex::filesystem::TruncateStdioFile(shader_storage_file_, 0);
    shader_storage_file_header.magic = shader_storage_magic;
    shader_storage_file_header.version_swapped = rex::byte_swap(ShaderStoredHeader::kVersion);
    fwrite(&shader_storage_file_header, sizeof(shader_storage_file_header), 1,
           shader_storage_file_);
  }

  // Create the pipelines.
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start_ = rex::chrono::Clock::QueryHostTickCount();

    // Launch additional creation threads to use all cores to create
    // pipelines faster. Will also be using the main thread, so minus 1.
    size_t creation_thread_original_count = creation_threads_.size();
    size_t creation_thread_needed_count =
        std::max(std::min(pipeline_stored_descriptions.size(), logical_processor_count) - size_t(1),
                 creation_thread_original_count);
    while (creation_threads_.size() < creation_thread_original_count) {
      size_t creation_thread_index = creation_threads_.size();
      std::unique_ptr<rex::thread::Thread> creation_thread = rex::thread::Thread::Create(
          {}, [this, creation_thread_index]() { CreationThread(creation_thread_index); });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }

    size_t pipelines_created = 0;
    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description = pipeline_stored_description.description;
      // TODO(Triang3l): On Vulkan, skip pipelines requiring unsupported device
      // features (to keep the cache files mostly shareable across devices).
      // Skip already known pipelines - those have already been enqueued.
      auto found_range = pipelines_.equal_range(pipeline_stored_description.description_hash);
      bool pipeline_found = false;
      for (auto it = found_range.first; it != found_range.second; ++it) {
        Pipeline* found_pipeline = it->second;
        if (!std::memcmp(&found_pipeline->description.description, &pipeline_description,
                         sizeof(pipeline_description))) {
          pipeline_found = true;
          break;
        }
      }
      if (pipeline_found) {
        continue;
      }

      PipelineRuntimeDescription pipeline_runtime_description;
      auto vertex_shader_it = shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        continue;
      }
      D3D12Shader* vertex_shader = vertex_shader_it->second;
      pipeline_runtime_description.vertex_shader = static_cast<D3D12Shader::D3D12Translation*>(
          vertex_shader->GetTranslation(pipeline_description.vertex_shader_modification));
      if (!pipeline_runtime_description.vertex_shader ||
          !pipeline_runtime_description.vertex_shader->is_translated() ||
          !pipeline_runtime_description.vertex_shader->is_valid()) {
        continue;
      }
      D3D12Shader* pixel_shader;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it = shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pipeline_runtime_description.pixel_shader = static_cast<D3D12Shader::D3D12Translation*>(
            pixel_shader->GetTranslation(pipeline_description.pixel_shader_modification));
        if (!pipeline_runtime_description.pixel_shader ||
            !pipeline_runtime_description.pixel_shader->is_translated() ||
            !pipeline_runtime_description.pixel_shader->is_valid()) {
          continue;
        }
      } else {
        pixel_shader = nullptr;
        pipeline_runtime_description.pixel_shader = nullptr;
      }
      GeometryShaderKey pipeline_geometry_shader_key;
      pipeline_runtime_description.geometry_shader =
          GetGeometryShaderKey(
              pipeline_description.geometry_shader,
              DxbcShaderTranslator::Modification(pipeline_description.vertex_shader_modification),
              DxbcShaderTranslator::Modification(pipeline_description.pixel_shader_modification),
              pipeline_geometry_shader_key)
              ? &GetGeometryShader(pipeline_geometry_shader_key)
              : nullptr;
      pipeline_runtime_description.root_signature = command_processor_.GetRootSignature(
          vertex_shader, pixel_shader,
          Shader::IsHostVertexShaderTypeDomain(
              DxbcShaderTranslator::Modification(pipeline_description.vertex_shader_modification)
                  .vertex.host_vertex_shader_type));
      if (!pipeline_runtime_description.root_signature) {
        continue;
      }
      std::memcpy(&pipeline_runtime_description.description, &pipeline_description,
                  sizeof(pipeline_description));

      Pipeline* new_pipeline = new Pipeline;
      std::memcpy(&new_pipeline->description, &pipeline_runtime_description,
                  sizeof(pipeline_runtime_description));
      new_pipeline->root_signature.store(pipeline_runtime_description.root_signature,
                                         std::memory_order_release);
      new_pipeline->metadata.origin = PipelineOrigin::kBootReplay;
      new_pipeline->metadata.first_frame.store(0, std::memory_order_relaxed);
      session_prewarmed_pipelines_.fetch_add(1, std::memory_order_relaxed);
      uint32_t bound_rts = 0;
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        if (pipeline_runtime_description.description.render_targets[i].used) {
          bound_rts |= uint32_t(1) << i;
        }
      }
      uint32_t shader_writes_color_targets =
          pipeline_runtime_description.pixel_shader
              ? pipeline_runtime_description.pixel_shader->shader().writes_color_targets()
              : 0;
      bool shader_writes_depth =
          pipeline_runtime_description.pixel_shader
              ? pipeline_runtime_description.pixel_shader->shader().writes_depth()
              : pipeline_runtime_description.description.depth_write != 0;
      new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
          bound_rts, shader_writes_color_targets, shader_writes_depth);
      pipelines_.emplace(pipeline_stored_description.description_hash, new_pipeline);
      COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());
      if (!creation_threads_.empty()) {
        // Submit the pipeline for creation to any available thread.
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_queue_.push(new_pipeline);
          PsoCompileIndicatorStateRef().creation_queue_size.store(
              uint32_t(creation_queue_.size()), std::memory_order_relaxed);
        }
        creation_request_cond_.notify_one();
      } else {
        new_pipeline->state.store(CreateD3D12Pipeline(pipeline_runtime_description),
                                  std::memory_order_release);
        if (progress.in_progress.load(std::memory_order_relaxed)) {
          progress.pipelines_created.fetch_add(1, std::memory_order_relaxed);
        }
      }
      ++pipelines_created;
      // Note: progress.pipelines_created is bumped from the actual compile
      // sites (CreationThread / CreateQueuedPipelinesOnProcessorThread / the
      // inline single-threaded branch above) so the overlay reflects driver
      // completion, not queue submission. Bumping here instead would saturate
      // the progress bar in <1s and then sit at 100% while the real work
      // continues for many seconds.
    }

    if (!creation_threads_.empty()) {
      CreateQueuedPipelinesOnProcessorThread();
      if (creation_threads_.size() > creation_thread_original_count) {
        {
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_threads_shutdown_from_ = creation_thread_original_count;
          // Assuming the queue is empty because of
          // CreateQueuedPipelinesOnProcessorThread.
        }
        creation_request_cond_.notify_all();
        while (creation_threads_.size() > creation_thread_original_count) {
          rex::thread::Wait(creation_threads_.back().get(), false);
          creation_threads_.pop_back();
        }
        bool await_creation_completion_event;
        {
          // Cleanup so additional threads can be created later again.
          std::lock_guard<std::mutex> lock(creation_request_lock_);
          creation_threads_shutdown_from_ = SIZE_MAX;
          // If the invocation is blocking, all the shader storage
          // initialization is expected to be done before proceeding, to avoid
          // latency in the command processor after the invocation.
          await_creation_completion_event = blocking && creation_threads_busy_ != 0;
          if (await_creation_completion_event) {
            creation_completion_event_->Reset();
            creation_completion_set_event_ = true;
          }
        }
        if (await_creation_completion_event) {
          creation_request_cond_.notify_one();
          rex::thread::Wait(creation_completion_event_.get(), false);
        }
      }
    }

    REXGPU_INFO(
        "Created {} graphics pipelines (not including reading the "
        "descriptions) from the storage in {} milliseconds",
        pipelines_created,
        (rex::chrono::Clock::QueryHostTickCount() - pipeline_creation_start_) * 1000 /
            rex::chrono::Clock::QueryHostTickFrequency());
    // If any pipeline descriptions were corrupted (or the whole file has excess
    // bytes in the end), truncate to the last valid pipeline description.
    rex::filesystem::TruncateStdioFile(
        pipeline_storage_file_,
        uint64_t(sizeof(pipeline_storage_file_header) +
                 sizeof(PipelineStoredDescription) * pipeline_stored_descriptions.size()));
  } else {
    rex::filesystem::TruncateStdioFile(pipeline_storage_file_, 0);
    pipeline_storage_file_header.magic = pipeline_storage_magic;
    pipeline_storage_file_header.magic_api = pipeline_storage_magic_api;
    pipeline_storage_file_header.version_swapped = pipeline_storage_version_swapped;
    fwrite(&pipeline_storage_file_header, sizeof(pipeline_storage_file_header), 1,
           pipeline_storage_file_);
  }

  shader_storage_cache_root_ = cache_root;
  shader_storage_title_id_ = title_id;

  // Start the storage writing thread.
  storage_write_flush_shaders_ = false;
  storage_write_flush_pipelines_ = false;
  storage_write_thread_shutdown_ = false;
  storage_write_thread_ = rex::thread::Thread::Create({}, [this]() { StorageWriteThread(); });
  assert_not_null(storage_write_thread_);
  storage_write_thread_->set_name("D3D12 Storage writer");

  // Report PSO library effectiveness after the cache replay so the user can
  // tell hot- vs cold-cache runs apart in the log without log_level=info.
  if (pipeline_library_) {
    REXGPU_WARN("PSO library status after init: {} loads (warm-boot hits), {} stores "
                "(new compiles added to library), {} failed-stores (duplicate or other)",
                pipeline_library_load_hits_.load(std::memory_order_relaxed),
                pipeline_library_store_count_.load(std::memory_order_relaxed),
                pipeline_library_store_failures_.load(std::memory_order_relaxed));
  }

  // Persist the PSO library now that all cache-replayed PSOs have been
  // created. This means even a hard-kill in mid-session preserves the
  // bulk-populated library blob; only mid-session newly stored PSOs (rare
  // after the first run) are lost on hard kill.
  FlushPipelineLibraryToDisk();

  // Also write the sidecar - identical defense against hard kills.
  WriteSidecarMetaTomlToDisk();

  // Drop the overlay. `finished` flips before `in_progress` so the dialog's
  // poll loop sees a consistent "done -> hide" transition even if it samples
  // between the two stores.
  progress.finished.store(true, std::memory_order_release);
  progress.in_progress.store(false, std::memory_order_release);
}

void PipelineCache::ShutdownShaderStorage() {
  if (storage_write_thread_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_thread_shutdown_ = true;
    }
    storage_write_request_cond_.notify_all();
    rex::thread::Wait(storage_write_thread_.get(), false);
    storage_write_thread_.reset();
  }
  storage_write_shader_queue_.clear();
  storage_write_pipeline_queue_.clear();

  if (pipeline_storage_file_) {
    fclose(pipeline_storage_file_);
    pipeline_storage_file_ = nullptr;
    pipeline_storage_file_flush_needed_ = false;
  }

  if (shader_storage_file_) {
    fclose(shader_storage_file_);
    shader_storage_file_ = nullptr;
    shader_storage_file_flush_needed_ = false;
  }

  // Write Phase-1 portable database sidecar.
  WriteSidecarMetaTomlToDisk();

  // Optional capture dump: when pso_capture_dump_on_exit=true, write a
  // human-readable list of every Pipeline observed this session with full
  // metadata. Suitable for diffing across runs and for hand-building a
  // "minimum shippable .xpso" by inspection.
  if (REXCVAR_GET(pso_capture_dump_on_exit) && !pipeline_metadata_sidecar_path_.empty()) {
    auto capture_path = pipeline_metadata_sidecar_path_;
    capture_path.replace_extension();  // strip .toml
    capture_path += ".capture.toml";
    FILE* cap_file = rex::filesystem::OpenFile(capture_path, "wb");
    if (cap_file) {
      const std::string header = fmt::format(
          "# Per-session PSO capture from rexglue-sdk PipelineCache.\n"
          "# Each [[pipeline]] = one unique Pipeline observed this session.\n"
          "title_id = 0x{:08X}\n"
          "pipeline_count = {}\n"
          "session_frames = {}\n\n",
          shader_storage_title_id_, pipelines_.size(),
          session_frame_counter_.load(std::memory_order_relaxed));
      fwrite(header.data(), 1, header.size(), cap_file);
      for (const auto& [hash, pipeline] : pipelines_) {
        const char* origin_str = "unknown";
        switch (pipeline->metadata.origin) {
          case PipelineOrigin::kBootReplay: origin_str = "boot_replay"; break;
          case PipelineOrigin::kRuntimeMiss: origin_str = "runtime_miss"; break;
          case PipelineOrigin::kUnknown: origin_str = "unknown"; break;
        }
        const auto& d = pipeline->description.description;
        const std::string entry = fmt::format(
            "[[pipeline]]\n"
            "hash = 0x{:016X}\n"
            "vs_hash = 0x{:016X}\n"
            "ps_hash = 0x{:016X}\n"
            "vs_modification = 0x{:016X}\n"
            "ps_modification = 0x{:016X}\n"
            "origin = \"{}\"\n"
            "caused_runtime_miss = {}\n"
            "usage_count = {}\n"
            "first_frame = {}\n"
            "last_frame = {}\n"
            "rt0_used = {}\n"
            "rt0_format = {}\n"
            "depth_format = {}\n"
            "msaa_samples = {}\n"
            "cull_mode = {}\n\n",
            hash, d.vertex_shader_hash, d.pixel_shader_hash, d.vertex_shader_modification,
            d.pixel_shader_modification, origin_str,
            pipeline->metadata.caused_runtime_miss ? "true" : "false",
            pipeline->metadata.usage_count.load(std::memory_order_relaxed),
            pipeline->metadata.first_frame.load(std::memory_order_relaxed),
            pipeline->metadata.last_frame.load(std::memory_order_relaxed),
            uint32_t(d.render_targets[0].used), uint32_t(d.render_targets[0].format),
            uint32_t(d.depth_format), uint32_t(d.host_msaa_samples), uint32_t(d.cull_mode));
        fwrite(entry.data(), 1, entry.size(), cap_file);
      }
      fclose(cap_file);
      REXGPU_WARN("PSO_DB: capture dump written to '{}' ({} pipelines)",
                  rex::path_to_utf8(capture_path), pipelines_.size());
    } else {
      REXGPU_WARN("PSO_DB: cannot open capture dump for write: {}",
                  rex::path_to_utf8(capture_path));
    }
  }

  // End-of-session summary. Aggregated counters span the entire session, not
  // just the most recent frame, so this is the line to grep when comparing
  // multiple runs. Skip when the call is the pre-Initialize cleanup (no
  // pipelines exist yet, no frames have ticked) - the line would be a row of
  // zeros and would only confuse the log reader.
  if (!pipelines_.empty() ||
      session_frame_counter_.load(std::memory_order_relaxed) > 0) {
    uint64_t boot_replay_count = 0;
    uint64_t boot_replay_used_count = 0;
    uint64_t runtime_miss_count = 0;
    for (const auto& [hash, pipeline] : pipelines_) {
      if (pipeline->metadata.origin == PipelineOrigin::kBootReplay) {
        ++boot_replay_count;
        if (pipeline->metadata.usage_count.load(std::memory_order_relaxed) > 0) {
          ++boot_replay_used_count;
        }
      } else if (pipeline->metadata.origin == PipelineOrigin::kRuntimeMiss) {
        ++runtime_miss_count;
      }
    }
    REXGPU_WARN(
        "PSO Summary: portable_keys_loaded={} prewarmed={} prewarmed_used={} "
        "runtime_misses={} new_keys_captured={} spikes_over_16ms={} spikes_over_100ms={} "
        "session_frames={} sidecar_mismatch={} total_pipelines={}",
        session_portable_keys_loaded_.load(std::memory_order_relaxed), boot_replay_count,
        boot_replay_used_count, runtime_miss_count,
        session_new_keys_captured_.load(std::memory_order_relaxed),
        session_spikes_over_16ms_.load(std::memory_order_relaxed),
        session_spikes_over_100ms_.load(std::memory_order_relaxed),
        session_frame_counter_.load(std::memory_order_relaxed),
        sidecar_metadata_mismatch_ ? "yes" : "no", pipelines_.size());
  }

  FlushPipelineLibraryToDisk();
  pipeline_library_.Reset();
  pipeline_library_blob_.clear();
  pipeline_library_path_.clear();
  pipeline_library_dirty_.store(false, std::memory_order_relaxed);

  shader_storage_cache_root_.clear();
  shader_storage_title_id_ = 0;
}

void PipelineCache::FlushPipelineLibraryToDisk() {
  if (!pipeline_library_ || !pipeline_library_dirty_.load(std::memory_order_relaxed) ||
      pipeline_library_path_.empty()) {
    return;
  }
  SIZE_T serialized_size;
  {
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    serialized_size = pipeline_library_->GetSerializedSize();
  }
  if (serialized_size == 0) {
    return;
  }
  std::vector<uint8_t> serialized(serialized_size);
  HRESULT hr;
  {
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    hr = pipeline_library_->Serialize(serialized.data(), serialized_size);
  }
  if (FAILED(hr)) {
    REXGPU_WARN("PSO library Serialize failed: HRESULT 0x{:08X}", uint32_t(hr));
    return;
  }
  auto tmp_path = pipeline_library_path_;
  tmp_path += ".tmp";
  FILE* lib_file = rex::filesystem::OpenFile(tmp_path, "wb");
  if (!lib_file) {
    REXGPU_WARN("PSO library file open failed: {}", rex::path_to_utf8(tmp_path));
    return;
  }
  size_t written = fwrite(serialized.data(), 1, serialized.size(), lib_file);
  fclose(lib_file);
  if (written != serialized.size()) {
    REXGPU_WARN("PSO library write incomplete ({}/{} bytes), discarding", written,
                serialized.size());
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    return;
  }
  std::error_code ec;
  std::filesystem::remove(pipeline_library_path_, ec);
  std::filesystem::rename(tmp_path, pipeline_library_path_, ec);
  if (ec) {
    REXGPU_WARN("PSO library rename failed: {}", ec.message());
    return;
  }
  pipeline_library_dirty_.store(false, std::memory_order_relaxed);
  REXGPU_WARN("PSO library flushed: {} bytes, {} stores, {} hits, {} failed-stores",
              serialized.size(),
              pipeline_library_store_count_.load(std::memory_order_relaxed),
              pipeline_library_load_hits_.load(std::memory_order_relaxed),
              pipeline_library_store_failures_.load(std::memory_order_relaxed));
}

void PipelineCache::WriteSidecarMetaTomlToDisk() {
  if (pipeline_metadata_sidecar_path_.empty() || shader_storage_title_id_ == 0) {
    return;
  }
  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  auto tmp_path = pipeline_metadata_sidecar_path_;
  tmp_path += ".tmp";
  FILE* meta_file = rex::filesystem::OpenFile(tmp_path, "wb");
  if (!meta_file) {
    REXGPU_WARN("PSO_DB: cannot open sidecar for write: {}",
                rex::path_to_utf8(pipeline_metadata_sidecar_path_));
    return;
  }
  const std::string text = fmt::format(
      "# Auto-generated by rexglue-sdk PipelineCache - do NOT bundle the "
      "matching .pso_lib in a release\n"
      "# (it is GPU/driver-specific). The .xpso file IS portable and can "
      "be shipped.\n"
      "title_id = 0x{:08X}\n"
      "description_version = 0x{:08X}\n"
      "modification_version = 0x{:08X}\n"
      "render_path = \"{}\"\n"
      "scale_x = {}\n"
      "scale_y = {}\n"
      "graphics_preset = \"default\"\n"
      "portable_keys_at_close = {}\n"
      "session_runtime_misses = {}\n",
      shader_storage_title_id_, PipelineDescription::kVersion,
      DxbcShaderTranslator::Modification::kVersion, edram_rov_used ? "rov" : "rtv",
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(), pipelines_.size(),
      session_runtime_misses_.load(std::memory_order_relaxed));
  fwrite(text.data(), 1, text.size(), meta_file);
  fclose(meta_file);
  std::error_code ec;
  std::filesystem::remove(pipeline_metadata_sidecar_path_, ec);
  std::filesystem::rename(tmp_path, pipeline_metadata_sidecar_path_, ec);
  if (ec) {
    REXGPU_WARN("PSO_DB: sidecar rename failed: {}", ec.message());
  }
}

void PipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ || pipeline_storage_file_flush_needed_) {
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      if (shader_storage_file_flush_needed_) {
        storage_write_flush_shaders_ = true;
      }
      if (pipeline_storage_file_flush_needed_) {
        storage_write_flush_pipelines_ = true;
      }
    }
    storage_write_request_cond_.notify_one();
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }
  if (!creation_threads_.empty()) {
    CreateQueuedPipelinesOnProcessorThread();
    // Decide whether to await in-flight PSO creation here. When the
    // d3d12_pso_no_block_at_submission_end cvar is true (default), we skip
    // the wait entirely — the still-compiling PSOs roll forward to whatever
    // frame finishes them, and any draw that hits a not-yet-ready PSO falls
    // into the pso_missing_policy path (default 'skip' = single-frame
    // missing-geometry artifact). This converts a per-frame 0-16ms wait
    // into a brief one-time pop-in per new PSO permutation.
    const bool no_block_at_submission_end =
        REXCVAR_GET(d3d12_pso_no_block_at_submission_end);
    bool await_creation_completion_event;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      // Assuming the creation queue is already empty (because the processor
      // thread also worked on creating the leftover pipelines), so only check
      // if there are threads with pipelines currently being created.
      await_creation_completion_event =
          !no_block_at_submission_end && creation_threads_busy_ != 0;
      if (await_creation_completion_event) {
        creation_completion_event_->Reset();
        creation_completion_set_event_ = true;
      }
    }
    if (await_creation_completion_event) {
      creation_request_cond_.notify_one();
      const double budget_ms = REXCVAR_GET(d3d12_pso_compile_budget_ms);
      if (budget_ms > 0.0) {
        // Bounded wait: cap render-thread blocking on background PSO
        // creation. On timeout, leftover PSOs stay in flight and the next
        // EndSubmission picks them up. The completion event was Reset()'d
        // above and creation_completion_set_event_ stays true, so whichever
        // background thread finishes the last queued pipeline still sets
        // the event for a future waiter to observe.
        auto budget = std::chrono::milliseconds(static_cast<int64_t>(budget_ms));
        auto wait_start = std::chrono::steady_clock::now();
        auto wait_result = rex::thread::Wait(creation_completion_event_.get(), false, budget);
        if (wait_result == rex::thread::WaitResult::kTimeout) {
          // Surface this in the per-frame log via the existing stall
          // metrics — count the timeout as a block_wait so the user can see
          // the budget actually kicked in.
          uint64_t wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - wait_start)
                                 .count();
          NoteRenderThreadBlockedOnAsyncPipeline(wait_ns);
        }
      } else {
        rex::thread::Wait(creation_completion_event_.get(), false);
      }
    }
  }
}

bool PipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void PipelineCache::ReportFrameBoundary(uint64_t frame_duration_ns) {
  // Per-session frame index, monotonic from 0 at startup. Used by Pipeline
  // metadata for first_frame / last_frame tracking; also feeds the summary
  // log's spike buckets. Increment FIRST so any usage in the same frame can
  // see the new value.
  const uint64_t frame_index = session_frame_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;
  // Spike buckets - count any frame visibly longer than 16 ms / 100 ms toward
  // the end-of-session summary, regardless of whether a PSO compile was the
  // cause. Lets the user spot trend changes across sessions.
  if (frame_duration_ns > 16'000'000ull) {
    session_spikes_over_16ms_.fetch_add(1, std::memory_order_relaxed);
  }
  if (frame_duration_ns > 100'000'000ull) {
    session_spikes_over_100ms_.fetch_add(1, std::memory_order_relaxed);
  }
  (void)frame_index;

  // Atomically drain per-frame counters so we don't double-count.
  const uint32_t misses = stall_metrics_.frame_misses.exchange(0, std::memory_order_acq_rel);
  const uint64_t sync_compile_ns =
      stall_metrics_.frame_sync_compile_ns.exchange(0, std::memory_order_acq_rel);
  const uint64_t async_compile_ns =
      stall_metrics_.frame_async_compile_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t async_completions =
      stall_metrics_.frame_async_completions.exchange(0, std::memory_order_acq_rel);
  const uint32_t draws_skipped =
      stall_metrics_.frame_draws_skipped.exchange(0, std::memory_order_acq_rel);
  const uint64_t block_wait_ns =
      stall_metrics_.frame_block_wait_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t blocks = stall_metrics_.frame_blocks.exchange(0, std::memory_order_acq_rel);
  const uint64_t submit_ns =
      stall_metrics_.frame_submit_ns.exchange(0, std::memory_order_acq_rel);
  const uint64_t present_ns =
      PsoStallPresentNsAccumulator().exchange(0, std::memory_order_acq_rel);
  const uint64_t vs_translate_ns =
      stall_metrics_.frame_vs_translate_ns.exchange(0, std::memory_order_acq_rel);
  const uint64_t ps_translate_ns =
      stall_metrics_.frame_ps_translate_ns.exchange(0, std::memory_order_acq_rel);
  // Non-PSO spike attribution (Etap 2). When pso_misses=0 yet frame_ms is
  // high, these counters identify which render-thread phase ate the budget.
  const uint64_t tex_req_ns =
      stall_metrics_.frame_texture_request_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t tex_req_calls =
      stall_metrics_.frame_texture_request_calls.exchange(0, std::memory_order_acq_rel);
  const uint64_t rt_resolve_ns =
      stall_metrics_.frame_rt_resolve_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t rt_resolve_count =
      stall_metrics_.frame_rt_resolve_count.exchange(0, std::memory_order_acq_rel);
  const uint64_t submit_barriers_ns =
      stall_metrics_.frame_submit_barriers_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t submit_barriers_calls =
      stall_metrics_.frame_submit_barriers_calls.exchange(0, std::memory_order_acq_rel);
  const uint64_t fence_wait_ns =
      stall_metrics_.frame_fence_wait_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t fence_waits =
      stall_metrics_.frame_fence_waits.exchange(0, std::memory_order_acq_rel);
  const uint64_t fence_wait_qops_ns =
      stall_metrics_.frame_fence_wait_qops_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t fence_wait_qops_count =
      stall_metrics_.frame_fence_wait_qops_count.exchange(0, std::memory_order_acq_rel);
  const uint64_t fence_wait_submission_ns =
      stall_metrics_.frame_fence_wait_submission_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t fence_wait_submission_count =
      stall_metrics_.frame_fence_wait_submission_count.exchange(0, std::memory_order_acq_rel);
  // Guest-CPU stall buckets (Class B investigation). These are written by the
  // kernel emulation layer (file I/O, heap allocator, NtWait*) running on the
  // guest thread, NOT the render thread. When a frame_ms spike fires with
  // render-thread buckets all near zero, large values here pinpoint which
  // kernel facility blocked the guest from producing Pm4 commands.
  auto& guest_cpu = ::rex::perf::GuestCpuStallMetricsRef();
  const uint64_t guest_io_ns =
      guest_cpu.frame_kernel_io_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t guest_io_calls =
      guest_cpu.frame_kernel_io_calls.exchange(0, std::memory_order_acq_rel);
  const uint64_t guest_alloc_ns =
      guest_cpu.frame_alloc_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t guest_alloc_calls =
      guest_cpu.frame_alloc_calls.exchange(0, std::memory_order_acq_rel);
  const uint64_t guest_wait_ns =
      guest_cpu.frame_wait_ns.exchange(0, std::memory_order_acq_rel);
  const uint32_t guest_wait_count =
      guest_cpu.frame_wait_count.exchange(0, std::memory_order_acq_rel);

  // Cheap exit: nothing happened on the compile path OR any non-PSO phase
  // this frame, AND submit / present were fast (sub-5 ms), AND no significant
  // guest-CPU stall (sub-5 ms per bucket).
  if (misses == 0 && sync_compile_ns == 0 && async_compile_ns == 0 && draws_skipped == 0 &&
      block_wait_ns == 0 && vs_translate_ns == 0 && ps_translate_ns == 0 &&
      submit_ns < 5'000'000ull && present_ns < 5'000'000ull &&
      tex_req_ns < 5'000'000ull && rt_resolve_ns < 5'000'000ull &&
      submit_barriers_ns < 5'000'000ull && fence_wait_ns < 5'000'000ull &&
      guest_io_ns < 5'000'000ull && guest_alloc_ns < 5'000'000ull &&
      guest_wait_ns < 5'000'000ull) {
    return;
  }

  const double frame_ms = static_cast<double>(frame_duration_ns) / 1e6;
  const double sync_compile_ms = static_cast<double>(sync_compile_ns) / 1e6;
  const double async_compile_ms = static_cast<double>(async_compile_ns) / 1e6;
  const double block_wait_ms = static_cast<double>(block_wait_ns) / 1e6;
  const double submit_ms = static_cast<double>(submit_ns) / 1e6;
  const double present_ms = static_cast<double>(present_ns) / 1e6;
  const double vs_translate_ms = static_cast<double>(vs_translate_ns) / 1e6;
  const double ps_translate_ms = static_cast<double>(ps_translate_ns) / 1e6;
  const double translate_ms = vs_translate_ms + ps_translate_ms;
  const double blocking_ms = sync_compile_ms + translate_ms + block_wait_ms;
  const double tex_req_ms = static_cast<double>(tex_req_ns) / 1e6;
  const double rt_resolve_ms = static_cast<double>(rt_resolve_ns) / 1e6;
  const double submit_barriers_ms = static_cast<double>(submit_barriers_ns) / 1e6;
  const double fence_wait_ms = static_cast<double>(fence_wait_ns) / 1e6;
  const double fence_wait_qops_ms = static_cast<double>(fence_wait_qops_ns) / 1e6;
  const double fence_wait_submission_ms = static_cast<double>(fence_wait_submission_ns) / 1e6;
  const double guest_io_ms = static_cast<double>(guest_io_ns) / 1e6;
  const double guest_alloc_ms = static_cast<double>(guest_alloc_ns) / 1e6;
  const double guest_wait_ms = static_cast<double>(guest_wait_ns) / 1e6;
  const double guest_cpu_total_ms = guest_io_ms + guest_alloc_ms + guest_wait_ms;
  // Accounted budget vs. wall-clock. Render-thread and guest-CPU buckets are
  // CONCURRENT on different threads, so we don't simply sum them — instead
  // report whichever dominated. unaccounted_ms = frame_ms - max(render, guest)
  // would be the cleanest model, but the cheap-and-clear approach is to keep
  // the original render-thread accounted total and report guest_cpu_total_ms
  // alongside. A high guest_cpu_total_ms with low render-thread totals
  // = guest thread starved the command processor (Class B).
  const double accounted_render_thread_ms = sync_compile_ms + translate_ms + block_wait_ms +
                                             submit_ms + present_ms + tex_req_ms + rt_resolve_ms +
                                             submit_barriers_ms + fence_wait_ms;
  const double accounted_max_ms = (accounted_render_thread_ms > guest_cpu_total_ms)
                                       ? accounted_render_thread_ms
                                       : guest_cpu_total_ms;
  const double unaccounted_ms = frame_ms - accounted_max_ms;

  // Severity bucket from frame wall-clock duration. CRITICAL > 250 ms is a
  // multi-frame freeze, EXTREME > 100 ms is a visible hitch, etc.
  //
  // 2026-06-27 (fix #5): bumped default MILD/HIGH thresholds behind
  // pso_stall_log_verbose. With Downpour sustaining median frame_ms = 33.6 ms
  // (30 FPS baseline) and triple-buffer + targeted memexport wait now keeping
  // fence_waits = 0-1 per frame in steady state, the 25 ms / 33 ms thresholds
  // tripped on every healthy 30 FPS frame and produced 800+ PSO_STALL events
  // per minute — pure log noise, no signal. Default now logs only SEVERE+
  // (> 50 ms — actual visible hitches). Verbose mode re-enables the MILD/HIGH
  // buckets for deep investigation.
  const bool verbose = REXCVAR_GET(pso_stall_log_verbose);
  const char* severity = nullptr;
  if (frame_ms > 250.0) {
    severity = "CRITICAL";
  } else if (frame_ms > 100.0) {
    severity = "EXTREME";
  } else if (frame_ms > 50.0) {
    severity = "SEVERE";
  } else if (verbose && frame_ms > 33.0) {
    severity = "HIGH";
  } else if (verbose && frame_ms > 25.0) {
    severity = "MILD";
  }

  // Conditional skip: only log if SOMETHING is interesting. This keeps the
  // log from being flooded by every frame that ran a tiny translate but was
  // otherwise smooth — that work is below the noise floor for stutter
  // analysis. Thresholds picked per user spec.
  // Primary triggers — always log these (genuinely abnormal frames):
  const bool log_for_frame_spike = severity != nullptr;             // frame_ms > 50 (or > 25 in verbose)
  const bool log_for_misses = misses > 0;                            // PSO had to compile
  const bool log_for_skipped = draws_skipped > 0;                   // missed PSOs in skip mode
  const bool log_for_block = block_wait_ms > 1.0;                   // block-mode wall-clock
  const bool log_for_compile = sync_compile_ms > 5.0 || async_compile_ms > 5.0;
  const bool log_for_translate = translate_ms > 5.0;                 // shader translate
  // Secondary triggers — these fire on every healthy GPU-bound frame (fence
  // wait > 5 ms is just "GPU is doing work"; submit/present/tex_req/rt_resolve
  // > 5 ms happens on big scenes by design). Useful when investigating a
  // specific spike, but as default warn lines they flood the log on every
  // frame at 15-25 ms and contribute their own CPU+IO cost. Gate them behind
  // the opt-in `pso_stall_log_verbose` cvar (read once above, near severity).
  const bool log_for_submit = verbose && submit_ms > 5.0;
  const bool log_for_present = verbose && present_ms > 5.0;
  const bool log_for_tex_req = verbose && tex_req_ms > 5.0;
  const bool log_for_rt_resolve = verbose && rt_resolve_ms > 5.0;
  const bool log_for_submit_barriers = verbose && submit_barriers_ms > 5.0;
  const bool log_for_fence_wait = verbose && fence_wait_ms > 5.0;
  // Guest-CPU stall trigger — PRIMARY (not verbose-gated) so Class B
  // investigation surfaces guest-thread block frames even if no render-thread
  // bucket is interesting. 5 ms threshold matches the secondary triggers.
  const bool log_for_guest_cpu = guest_cpu_total_ms > 5.0;
  if (!(log_for_frame_spike || log_for_misses || log_for_compile || log_for_translate ||
        log_for_skipped || log_for_block || log_for_submit || log_for_present ||
        log_for_tex_req || log_for_rt_resolve || log_for_submit_barriers || log_for_fence_wait ||
        log_for_guest_cpu)) {
    return;
  }

  const uint64_t blocking_pso = stall_metrics_.last_blocking_pso_hash.load(std::memory_order_relaxed);
  const uint64_t blocking_vs = stall_metrics_.last_blocking_vs_hash.load(std::memory_order_relaxed);
  const uint64_t blocking_ps = stall_metrics_.last_blocking_ps_hash.load(std::memory_order_relaxed);
  const uint32_t total_session =
      stall_metrics_.total_misses_session.load(std::memory_order_relaxed);

  // Render path string for the human reader.
  const char* render_path =
      render_target_cache_.GetPath() == ::rex::graphics::RenderTargetCache::Path::kPixelShaderInterlock
          ? "ROV"
          : "RTV";

  // Snapshot the last blocking description (RT format / depth / blend).
  PipelineDescription desc{};
  bool desc_valid = false;
  {
    std::lock_guard<std::mutex> lock(last_blocking_description_mutex_);
    if (last_blocking_description_valid_) {
      desc = last_blocking_description_;
      desc_valid = true;
    }
  }

  // Build the RT[0]/depth summary if we have a description. The full
  // PipelineDescription is XXH3'd to produce blocking_pso_hash so a single
  // hex line is enough to identify the PSO for a prewarm pass, but a few
  // human-readable fields help spot patterns (e.g. all spikes hit on a
  // particular RT format).
  uint32_t rt0_format = 0;
  uint32_t rt0_used = 0;
  uint32_t depth_format = 0;
  uint32_t depth_func = 0;
  uint32_t depth_write = 0;
  uint32_t stencil_enable = 0;
  uint32_t msaa_samples = 0;
  uint32_t cull_mode = 0;
  if (desc_valid) {
    rt0_format = static_cast<uint32_t>(desc.render_targets[0].format);
    rt0_used = desc.render_targets[0].used;
    depth_format = static_cast<uint32_t>(desc.depth_format);
    depth_func = static_cast<uint32_t>(desc.depth_func);
    depth_write = desc.depth_write;
    stencil_enable = desc.stencil_enable;
    msaa_samples = static_cast<uint32_t>(desc.host_msaa_samples);
    cull_mode = static_cast<uint32_t>(desc.cull_mode);
  }

  // The "blocking_compile" flag answers the headline question: did the
  // render thread spend non-trivial wall-clock on a synchronous compile this
  // frame? Anything > 1 ms is a stall by 60 FPS budget standards.
  const bool blocking_compile = sync_compile_ms > 1.0 || translate_ms > 1.0 || block_wait_ms > 1.0;

  // Read the current PSO-missing policy for the log line (skip/block/sync).
  const auto& policy_cvar = REXCVAR_GET(pso_missing_policy);
  const char* policy_label = "skip";
  if (policy_cvar == "block") {
    policy_label = "block";
  } else if (policy_cvar == "sync") {
    policy_label = "sync";
  }

  const char* label =
      severity ? severity
               : (log_for_skipped ? "skipped-draws"
                                  : (log_for_block ? "blocked"
                                                   : (log_for_guest_cpu ? "guest-cpu-stall"
                                                                        : (log_for_submit || log_for_present
                                                                               ? "submit-present"
                                                                               : "compile-only"))));

  // Epic-style PSO precache validation label (Hit/Missed/Too-late terminology
  // from the docx analysis): classify each miss according to whether it
  // happened during the warmup window or after it. Pre-warmup misses are
  // expected (cold cache loading); post-warmup misses that ALSO produced a
  // user-visible spike are "TOO-LATE" — we should have precompiled them
  // during the shader compile dialog but didn't. Pure POST-WARMUP-MISS (no
  // spike severity) is benign — the PSO is being lazily fetched in the
  // background and the user didn't notice.
  auto& shader_progress = command_processor_.graphics_system()->shader_storage_progress();
  const bool warmup_in_progress = shader_progress.in_progress.load(std::memory_order_relaxed);
  const bool warmup_finished = shader_progress.finished.load(std::memory_order_relaxed);
  const char* validation_label = "hit";
  if (misses > 0) {
    if (warmup_in_progress || !warmup_finished) {
      validation_label = "startup-miss";
    } else if (severity != nullptr) {
      validation_label = "too-late";
    } else {
      validation_label = "post-warmup-miss";
    }
  }
  const uint32_t spike_idx = spike_frame_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  REXGPU_WARN(
      "PSO_STALL[{}] #{} validation={} frame_ms={:.2f} render_path={} pso_missing_policy={} "
      "pso_misses={} vs_translate_ms={:.2f} ps_translate_ms={:.2f} "
      "sync_compile_ms={:.2f} async_compile_ms={:.2f} async_completions={} "
      "draws_skipped_awaiting_pso={} block_wait_ms={:.2f} blocks={} "
      "submit_ms={:.2f} present_ms={:.2f} "
      "tex_req_ms={:.2f} tex_req_calls={} "
      "rt_resolve_ms={:.2f} rt_resolve_count={} "
      "submit_barriers_ms={:.2f} submit_barriers_calls={} "
      "fence_wait_ms={:.2f} fence_waits={} "
      "fence_wait_qops_ms={:.2f} fence_wait_qops={} "
      "fence_wait_submission_ms={:.2f} fence_wait_submission={} "
      "guest_cpu_io_ms={:.2f} guest_cpu_io_calls={} "
      "guest_cpu_alloc_ms={:.2f} guest_cpu_alloc_calls={} "
      "guest_cpu_wait_ms={:.2f} guest_cpu_wait_count={} "
      "guest_cpu_total_ms={:.2f} "
      "unaccounted_ms={:.2f} "
      "blocking_on_render_thread_ms={:.2f} blocking_compile={} "
      "blocking_pso_hash=0x{:016x} vs_hash=0x{:016x} ps_hash=0x{:016x} "
      "rt0_used={} rt0_format={} depth_format={} depth_func={} depth_write={} "
      "stencil_enable={} msaa_samples={} cull_mode={} "
      "session_total_misses={}",
      label, spike_idx, validation_label, frame_ms, render_path, policy_label, misses,
      vs_translate_ms, ps_translate_ms, sync_compile_ms, async_compile_ms, async_completions,
      draws_skipped, block_wait_ms, blocks, submit_ms, present_ms,
      tex_req_ms, tex_req_calls,
      rt_resolve_ms, rt_resolve_count,
      submit_barriers_ms, submit_barriers_calls,
      fence_wait_ms, fence_waits,
      fence_wait_qops_ms, fence_wait_qops_count,
      fence_wait_submission_ms, fence_wait_submission_count,
      guest_io_ms, guest_io_calls,
      guest_alloc_ms, guest_alloc_calls,
      guest_wait_ms, guest_wait_count,
      guest_cpu_total_ms,
      unaccounted_ms,
      blocking_ms,
      blocking_compile ? "yes" : "no", blocking_pso, blocking_vs, blocking_ps, rt0_used,
      rt0_format, depth_format, depth_func, depth_write, stencil_enable, msaa_samples, cull_mode,
      total_session);
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                                       uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                                       uint32_t dword_count, uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  D3D12Shader* shader = new D3D12Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

DxbcShaderTranslator::Modification PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  modification.vertex.vertex_kill_and = uint32_t(
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      modification.pixel.depth_stencil_mode = render_target_cache_.depth_float24_round()
                                                  ? DepthStencilMode::kFloat24Rounding
                                                  : DepthStencilMode::kFloat24Truncating;
    } else {
      if (shader.implicit_early_z_write_allowed() &&
          (!shader.writes_color_target(0) ||
           !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }
  }

  return modification;
}

bool PipelineCache::ConfigurePipeline(
    D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats, void** pipeline_handle_out,
    ID3D12RootSignature** root_signature_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  assert_not_null(pipeline_handle_out);
  assert_not_null(root_signature_out);

  bool use_async = REXCVAR_GET(async_shader_compilation) && !creation_threads_.empty() &&
                   pixel_shader != nullptr;
  // pso_missing_policy == "sync" forces synchronous compile path regardless
  // of the async_shader_compilation default, so the render thread blocks
  // directly on CreateD3D12Pipeline. Used as a control test.
  if (use_async && REXCVAR_GET(pso_missing_policy) == "sync") {
    use_async = false;
  }

  // Ensure shaders are translated - needed now for GetCurrentStateDescription.
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);
  // Ucode analysis is always needed on the main thread (for modification and
  // hash computation). Translation can be deferred to background threads when
  // async compilation is enabled.
  if (!vertex_shader->shader().is_ucode_analyzed()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (!vertex_shader->is_translated() && !use_async) {
    std::lock_guard<std::mutex> lock(translation_request_lock_);
    if (!vertex_shader->is_translated()) {
      auto translate_start = std::chrono::steady_clock::now();
      bool translate_ok = TranslateAnalyzedShader(*shader_translator_, *vertex_shader,
                                                  dxbc_converter_, dxc_utils_, dxc_compiler_);
      stall_metrics_.frame_vs_translate_ns.fetch_add(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - translate_start)
              .count(),
          std::memory_order_relaxed);
      if (!translate_ok) {
        REXGPU_ERROR("Failed to translate the vertex shader!");
        return false;
      }
      if (shader_storage_file_ &&
          vertex_shader->shader().ucode_storage_index() != shader_storage_index_) {
        vertex_shader->shader().set_ucode_storage_index(shader_storage_index_);
        assert_not_null(storage_write_thread_);
        shader_storage_file_flush_needed_ = true;
        {
          std::lock_guard<std::mutex> storage_lock(storage_write_request_lock_);
          storage_write_shader_queue_.push_back(&vertex_shader->shader());
        }
        storage_write_request_cond_.notify_all();
      }
    }
  }
  if (!use_async && !vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid.
    return false;
  }
  if (pixel_shader != nullptr) {
    if (!pixel_shader->is_translated() && !use_async) {
      std::lock_guard<std::mutex> lock(translation_request_lock_);
      if (!pixel_shader->is_translated()) {
        pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
        auto translate_start = std::chrono::steady_clock::now();
        bool translate_ok = TranslateAnalyzedShader(*shader_translator_, *pixel_shader,
                                                    dxbc_converter_, dxc_utils_, dxc_compiler_);
        stall_metrics_.frame_ps_translate_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - translate_start)
                .count(),
            std::memory_order_relaxed);
        if (!translate_ok) {
          REXGPU_ERROR("Failed to translate the pixel shader!");
          return false;
        }
        if (shader_storage_file_ &&
            pixel_shader->shader().ucode_storage_index() != shader_storage_index_) {
          pixel_shader->shader().set_ucode_storage_index(shader_storage_index_);
          assert_not_null(storage_write_thread_);
          shader_storage_file_flush_needed_ = true;
          {
            std::lock_guard<std::mutex> storage_lock(storage_write_request_lock_);
            storage_write_shader_queue_.push_back(&pixel_shader->shader());
          }
          storage_write_request_cond_.notify_all();
        }
      }
    }
    if (pixel_shader->is_translated() && !pixel_shader->is_valid()) {
      return false;
    }
  }

  PipelineRuntimeDescription runtime_description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result, normalized_depth_control,
          normalized_color_mask, bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, runtime_description, use_async)) {
    return false;
  }
  PipelineDescription& description = runtime_description.description;

  if (current_pipeline_ != nullptr && !std::memcmp(&current_pipeline_->description.description,
                                                   &description, sizeof(description))) {
    // Same-pipeline fast path: still update usage_count for accurate summary.
    current_pipeline_->metadata.usage_count.fetch_add(1, std::memory_order_relaxed);
    current_pipeline_->metadata.last_frame.store(
        session_frame_counter_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    *pipeline_handle_out = current_pipeline_;
    *root_signature_out = current_pipeline_->root_signature.load(std::memory_order_acquire);
    return true;
  }

  // Find an existing pipeline in the cache.
  uint64_t hash = XXH3_64bits(&description, sizeof(description));
  auto found_range = pipelines_.equal_range(hash);
  for (auto it = found_range.first; it != found_range.second; ++it) {
    Pipeline* found_pipeline = it->second;
    if (!std::memcmp(&found_pipeline->description.description, &description, sizeof(description))) {
      PROFILE_PIPELINE_CACHE_HIT();
      // Live usage tracking for the end-of-session summary + capture file.
      found_pipeline->metadata.usage_count.fetch_add(1, std::memory_order_relaxed);
      found_pipeline->metadata.last_frame.store(
          session_frame_counter_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      current_pipeline_ = found_pipeline;
      *pipeline_handle_out = found_pipeline;
      *root_signature_out = found_pipeline->root_signature.load(std::memory_order_acquire);
      return true;
    }
  }
  PROFILE_PIPELINE_CACHE_MISS();

  // Count the miss BEFORE branching into sync vs async so the per-frame log
  // reflects all cache misses, not only the rare synchronous ones (Windows
  // default has async_shader_compilation = true → almost all misses take the
  // async branch and would otherwise be invisible).
  stall_metrics_.frame_misses.fetch_add(1, std::memory_order_relaxed);
  stall_metrics_.total_misses_session.fetch_add(1, std::memory_order_relaxed);
  PsoCompileIndicatorStateRef().total_misses_session.fetch_add(1, std::memory_order_relaxed);
  stall_metrics_.last_blocking_pso_hash.store(hash, std::memory_order_relaxed);
  stall_metrics_.last_blocking_vs_hash.store(description.vertex_shader_hash,
                                             std::memory_order_relaxed);
  stall_metrics_.last_blocking_ps_hash.store(description.pixel_shader_hash,
                                             std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(last_blocking_description_mutex_);
    last_blocking_description_ = description;
    last_blocking_description_valid_ = true;
  }

  Pipeline* new_pipeline = new Pipeline;
  std::memcpy(&new_pipeline->description, &runtime_description, sizeof(runtime_description));
  new_pipeline->root_signature.store(runtime_description.root_signature, std::memory_order_release);
  new_pipeline->metadata.origin = PipelineOrigin::kRuntimeMiss;
  new_pipeline->metadata.caused_runtime_miss = true;
  new_pipeline->metadata.first_frame.store(session_frame_counter_.load(std::memory_order_relaxed),
                                           std::memory_order_relaxed);
  session_runtime_misses_.fetch_add(1, std::memory_order_relaxed);
  session_new_keys_captured_.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(pso_capture)) {
    // Demoted from WARN → INFO 2026-06-22: at log_level=warn this fires for
    // every new PSO and floods the trace overlay during normal gameplay. The
    // capture sidecar (.xpso.capture.toml on shutdown) still records every
    // key — only the per-miss WARN line goes away.
    REXGPU_INFO(
        "PSO_CAPTURE: new key hash=0x{:016X} vs=0x{:016X} ps=0x{:016X} "
        "rt0_used={} rt0_format={} depth_format={} depth_func={} depth_write={} "
        "msaa_samples={} cull_mode={} frame={}",
        hash, description.vertex_shader_hash, description.pixel_shader_hash,
        uint32_t(description.render_targets[0].used),
        uint32_t(description.render_targets[0].format), uint32_t(description.depth_format),
        uint32_t(description.depth_func), uint32_t(description.depth_write),
        uint32_t(description.host_msaa_samples), uint32_t(description.cull_mode),
        session_frame_counter_.load(std::memory_order_relaxed));
  }
  pipelines_.emplace(hash, new_pipeline);
  COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());

  if (use_async) {
    uint32_t bound_rts =
        pipeline_util::GetBoundRTMaskFromNormalizedColorMask(normalized_color_mask);
    uint32_t shader_writes_color_targets =
        pixel_shader ? pixel_shader->shader().writes_color_targets() : 0;
    bool shader_writes_depth = pixel_shader ? pixel_shader->shader().writes_depth()
                                            : normalized_depth_control.z_write_enable != 0;
    new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
        bound_rts, shader_writes_color_targets, shader_writes_depth);
    new_pipeline->pending_vertex_shader = vertex_shader;
    new_pipeline->pending_pixel_shader = pixel_shader;
    // Submit the pipeline for creation to any available thread.
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      creation_queue_.push(new_pipeline);
      PsoCompileIndicatorStateRef().creation_queue_size.store(
          uint32_t(creation_queue_.size()), std::memory_order_relaxed);
    }
    creation_request_cond_.notify_one();
  } else {
    // Synchronous compilation on the command-processor thread — wall-clock
    // here is the direct render-thread stall measurement.
    auto compile_start = std::chrono::steady_clock::now();
    ID3D12PipelineState* pipeline_state = CreateD3D12Pipeline(runtime_description);
    auto compile_end = std::chrono::steady_clock::now();
    uint64_t compile_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(compile_end - compile_start).count();
    new_pipeline->state.store(pipeline_state, std::memory_order_release);
    stall_metrics_.frame_sync_compile_ns.fetch_add(compile_ns, std::memory_order_relaxed);
    // Mirror to the cross-module indicator state so the overlay can show a
    // post-stall flash for sync mode (otherwise it stays empty under
    // pso_missing_policy='sync' because there's no async queue).
    auto& indicator = PsoCompileIndicatorStateRef();
    indicator.sync_compiles_session.fetch_add(1, std::memory_order_relaxed);
    indicator.last_sync_compile_steady_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(compile_end.time_since_epoch()).count(),
        std::memory_order_relaxed);
    float compile_ms = static_cast<float>(static_cast<double>(compile_ns) / 1.0e6);
    uint32_t bits;
    std::memcpy(&bits, &compile_ms, sizeof(bits));
    indicator.last_sync_compile_duration_ms_bits.store(bits, std::memory_order_relaxed);
  }

  if (pipeline_storage_file_) {
    assert_not_null(storage_write_thread_);
    pipeline_storage_file_flush_needed_ = true;
    {
      std::lock_guard<std::mutex> lock(storage_write_request_lock_);
      storage_write_pipeline_queue_.emplace_back();
      PipelineStoredDescription& stored_description = storage_write_pipeline_queue_.back();
      stored_description.description_hash = hash;
      std::memcpy(&stored_description.description, &description, sizeof(description));
    }
    storage_write_request_cond_.notify_all();
  }

  current_pipeline_ = new_pipeline;
  *pipeline_handle_out = new_pipeline;
  *root_signature_out = new_pipeline->root_signature.load(std::memory_order_acquire);
  return true;
}

bool PipelineCache::TranslateAnalyzedShader(DxbcShaderTranslator& translator,
                                            D3D12Shader::D3D12Translation& translation,
                                            IDxbcConverter* dxbc_converter, IDxcUtils* dxc_utils,
                                            IDxcCompiler* dxc_compiler) {
  D3D12Shader& shader = static_cast<D3D12Shader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("Shader {:016X} translation failed; marking as ignored", shader.ucode_data_hash());
    return false;
  }

  const char* host_shader_type;
  if (shader.type() == xenos::ShaderType::kVertex) {
    DxbcShaderTranslator::Modification modification(translation.modification());
    switch (modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        host_shader_type = "control-point-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        host_shader_type = "patch-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        host_shader_type = "control-point-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        host_shader_type = "patch-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        host_shader_type = "control-point-indexed quad domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        host_shader_type = "patch-indexed quad domain";
        break;
      default:
        assert(modification.vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kVertex);
        host_shader_type = "vertex";
    }
  } else {
    host_shader_type = "pixel";
  }
  REXGPU_NOISY_DEBUG("Generated {} shader ({}b) - hash {:016X}:\n{}\n", host_shader_type,
                     shader.ucode_dword_count() * sizeof(uint32_t), shader.ucode_data_hash(),
                     shader.ucode_disassembly().c_str());

  // Set up texture and sampler binding layouts.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    const std::vector<D3D12Shader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    const std::vector<D3D12Shader::SamplerBinding>& sampler_bindings =
        shader.GetSamplerBindingsAfterTranslation();
    size_t sampler_binding_count = sampler_bindings.size();
    assert_false(bindless_resources_used_ && texture_binding_count + sampler_binding_count >
                                                 D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4);
    size_t texture_binding_layout_bytes = texture_binding_count * sizeof(*texture_bindings.data());
    uint64_t texture_binding_layout_hash = 0;
    if (texture_binding_count) {
      texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
    }
    size_t bindless_sampler_count = bindless_resources_used_ ? sampler_binding_count : 0;
    uint64_t bindless_sampler_layout_hash = 0;
    if (bindless_sampler_count) {
      XXH3_state_t hash_state;
      XXH3_64bits_reset(&hash_state);
      for (size_t i = 0; i < bindless_sampler_count; ++i) {
        XXH3_64bits_update(&hash_state, &sampler_bindings[i].bindless_descriptor_index,
                           sizeof(sampler_bindings[i].bindless_descriptor_index));
      }
      bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
    }
    // Obtain the unique IDs of binding layouts if there are any texture
    // bindings or bindless samplers, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    // Use sampler count for the bindful case because it's the only thing that
    // must be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for actual
    // samplers).
    static_assert(kLayoutUIDEmpty == 0,
                  "Empty layout UID is assumed to be 0 because for bindful samplers, the "
                  "UID is their count");
    size_t sampler_binding_layout_uid =
        bindless_resources_used_ ? kLayoutUIDEmpty : sampler_binding_count;
    if (texture_binding_count || bindless_sampler_count) {
      std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
      if (texture_binding_count) {
        auto found_range = texture_binding_layout_map_.equal_range(texture_binding_layout_hash);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length == texture_binding_count &&
              !std::memcmp(texture_binding_layouts_.data() + it->second.vector_span_offset,
                           texture_bindings.data(), texture_binding_layout_bytes)) {
            texture_binding_layout_uid = it->second.uid;
            break;
          }
        }
        if (texture_binding_layout_uid == kLayoutUIDEmpty) {
          static_assert(kLayoutUIDEmpty == 0,
                        "Layout UID is size + 1 because it's assumed that 0 is the UID "
                        "for an empty layout");
          texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
          LayoutUID new_uid;
          new_uid.uid = texture_binding_layout_uid;
          new_uid.vector_span_offset = texture_binding_layouts_.size();
          new_uid.vector_span_length = texture_binding_count;
          texture_binding_layouts_.resize(new_uid.vector_span_offset + texture_binding_count);
          std::memcpy(texture_binding_layouts_.data() + new_uid.vector_span_offset,
                      texture_bindings.data(), texture_binding_layout_bytes);
          texture_binding_layout_map_.emplace(texture_binding_layout_hash, new_uid);
        }
      }
      if (bindless_sampler_count) {
        auto found_range = bindless_sampler_layout_map_.equal_range(sampler_binding_layout_uid);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length != bindless_sampler_count) {
            continue;
          }
          sampler_binding_layout_uid = it->second.uid;
          const uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + it->second.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            if (vector_bindless_sampler_layout[i] !=
                sampler_bindings[i].bindless_descriptor_index) {
              sampler_binding_layout_uid = kLayoutUIDEmpty;
              break;
            }
          }
          if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
            break;
          }
        }
        if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
          sampler_binding_layout_uid = bindless_sampler_layout_map_.size();
          LayoutUID new_uid;
          static_assert(kLayoutUIDEmpty == 0,
                        "Layout UID is size + 1 because it's assumed that 0 is the UID "
                        "for an empty layout");
          new_uid.uid = sampler_binding_layout_uid + 1;
          new_uid.vector_span_offset = bindless_sampler_layouts_.size();
          new_uid.vector_span_length = sampler_binding_count;
          bindless_sampler_layouts_.resize(new_uid.vector_span_offset + sampler_binding_count);
          uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            vector_bindless_sampler_layout[i] = sampler_bindings[i].bindless_descriptor_index;
          }
          bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash, new_uid);
        }
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
    shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
  }

  // Disassemble the shader for dumping.
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (REXCVAR_GET(d3d12_dxbc_disasm_dxilconv)) {
    translation.DisassembleDxbcAndDxil(provider, REXCVAR_GET(d3d12_dxbc_disasm), dxbc_converter,
                                       dxc_utils, dxc_compiler);
  } else {
    translation.DisassembleDxbcAndDxil(provider, REXCVAR_GET(d3d12_dxbc_disasm));
  }

  // Dump shader files if desired.
  if (!REXCVAR_GET(dump_shaders).empty()) {
    bool edram_rov_used =
        render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
    translation.Dump(REXCVAR_GET(dump_shaders), (shader.type() == xenos::ShaderType::kPixel)
                                                    ? (edram_rov_used ? "d3d12_rov" : "d3d12_rtv")
                                                    : "d3d12");
  }

  return translation.is_valid();
}

bool PipelineCache::GetCurrentStateDescription(
    D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    PipelineRuntimeDescription& runtime_description_out, bool for_placeholder) {
  // Translated shaders needed at least for the root signature, unless in
  // placeholder mode (async compilation) where both VS and PS translation
  // may be deferred to background threads.
  assert_true(for_placeholder || (vertex_shader->is_translated() && vertex_shader->is_valid()));
  assert_true(for_placeholder || !pixel_shader ||
              (pixel_shader->is_translated() && pixel_shader->is_valid()));

  PipelineDescription& description_out = runtime_description_out.description;

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  // Initialize all unused fields to zero for comparison/hashing.
  std::memset(&runtime_description_out, 0, sizeof(runtime_description_out));

  assert_true(DxbcShaderTranslator::Modification(vertex_shader->modification())
                  .vertex.host_vertex_shader_type ==
              primitive_processing_result.host_vertex_shader_type);
  bool tessellated = primitive_processing_result.IsTessellated();
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  // In Direct3D, rasterization (along with pixel counting) is disabled by
  // disabling the pixel shader and depth / stencil. However, if rasterization
  // should be disabled, the pixel shader must be disabled externally, to ensure
  // things like texture binding layout is correct for the shader actually being
  // used (don't replace anything here).
  if (!rasterization_enabled) {
    assert_null(pixel_shader);
    if (pixel_shader) {
      return false;
    }
  }

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  runtime_description_out.root_signature = command_processor_.GetRootSignature(
      static_cast<const DxbcShader*>(&vertex_shader->shader()),
      (pixel_shader && !for_placeholder) ? static_cast<const DxbcShader*>(&pixel_shader->shader())
                                         : nullptr,
      tessellated);
  if (runtime_description_out.root_signature == nullptr) {
    return false;
  }

  // Vertex shader.
  runtime_description_out.vertex_shader = vertex_shader;
  description_out.vertex_shader_hash = vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();

  // Index buffer strip cut value.
  if (primitive_processing_result.host_primitive_reset_enabled) {
    description_out.strip_cut_index =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? PipelineStripCutIndex::kFFFF
            : PipelineStripCutIndex::kFFFFFFFF;
  } else {
    description_out.strip_cut_index = PipelineStripCutIndex::kNone;
  }

  // Host vertex shader type and primitive topology.
  if (tessellated) {
    description_out.primitive_topology_type_or_tessellation_mode =
        uint32_t(primitive_processing_result.tessellation_mode);
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader = PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  }
  GeometryShaderKey geometry_shader_key;
  runtime_description_out.geometry_shader =
      GetGeometryShaderKey(
          description_out.geometry_shader,
          DxbcShaderTranslator::Modification(vertex_shader->modification()),
          DxbcShaderTranslator::Modification(pixel_shader ? pixel_shader->modification() : 0),
          geometry_shader_key)
          ? &GetGeometryShader(geometry_shader_key)
          : nullptr;

  // The rest doesn't matter when rasterization is disabled (thus no writing to
  // anywhere from post-geometry stages and no samples are counted).
  if (!rasterization_enabled) {
    description_out.cull_mode = PipelineCullMode::kDisableRasterization;
    return true;
  }

  // Pixel shader.
  if (pixel_shader) {
    runtime_description_out.pixel_shader = pixel_shader;
    description_out.pixel_shader_hash = pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }

  // Rasterizer state.
  // Because Direct3D 12 doesn't support per-side fill mode and depth bias, the
  // values to use depends on the current culling state.
  // If front faces are culled, use the ones for back faces.
  // If back faces are culled, it's the other way around.
  // If culling is not enabled, assume the developer wanted to draw things in a
  // more special way - so if one side is wireframe or has a depth bias, then
  // that's intentional (if both sides have a depth bias, the one for the front
  // faces is used, though it's unlikely that they will ever be different -
  // SetRenderState sets the same offset for both sides).
  // Points fill mode (0) also isn't supported in Direct3D 12, but assume the
  // developer didn't want to fill the whole primitive and use wireframe (like
  // Xenos fill mode 1).
  // Here we also assume that only one side is culled - if two sides are culled,
  // rasterization will be disabled externally, or the draw call will be dropped
  // early if the vertex shader doesn't export to memory.
  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      // The case when both faces are culled should be handled by disabling
      // rasterization.
      assert_false(cull_back);
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    // With ROV, the depth bias is applied in the pixel shader because
    // per-sample depth is needed for MSAA.
    if (!cull_front) {
      // Front faces aren't culled.
      // Direct3D 12, unfortunately, doesn't support point fill mode.
      if (pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (!cull_back) {
      // Back faces aren't culled.
      if (pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    // Filled front faces only, without culling.
    cull_front = false;
    cull_back = false;
  }
  if (!edram_rov_used) {
    float polygon_offset, polygon_offset_scale;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                             polygon_offset);
    description_out.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    description_out.depth_bias_slope_scaled =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
  }
  if (tessellated && REXCVAR_GET(d3d12_tessellation_wireframe)) {
    description_out.fill_mode_wireframe = 1;
  }
  description_out.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  bool depth_stencil_bound_and_used = false;
  if (!edram_rov_used) {
    // Depth/stencil. No stencil, always passing depth test and no depth writing
    // means depth disabled.
    if (bound_depth_and_color_render_target_bits & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_func = normalized_depth_control.zfunc;
        description_out.depth_write = normalized_depth_control.z_write_enable;
      } else {
        description_out.depth_func = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_enable = 1;
        bool stencil_backface_enable =
            primitive_polygonal && normalized_depth_control.backface_enable;
        // Per-face masks not supported by Direct3D 12, choose the back face
        // ones only if drawing only back faces.
        Register stencil_ref_mask_reg;
        if (stencil_backface_enable && cull_front) {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
        }
        auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
        description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
        description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
        description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
        description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
        description_out.stencil_front_func = normalized_depth_control.stencilfunc;
        if (stencil_backface_enable) {
          description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_func = normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
          description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
          description_out.stencil_back_func = description_out.stencil_front_func;
        }
      }
      // If not binding the DSV, ignore the format in the hash.
      if (description_out.depth_func != xenos::CompareFunction::kAlways ||
          description_out.depth_write || description_out.stencil_enable) {
        description_out.depth_format =
            xenos::DepthRenderTargetFormat(bound_depth_and_color_render_target_formats[0]);
        depth_stencil_bound_and_used = true;
      }
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }

    // Render targets and blending state. 32 because of 0x1F mask, for safety
    // (all unknown to zero).
    static const PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kInvSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestColor,
        /*  9 */ PipelineBlendFactor::kInvDestColor,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        // CONSTANT_COLOR
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // Like kBlendFactorMap, but with color modes changed to alpha. Some
    // pipelines aren't created in 545407E0 because a color mode is used for
    // alpha.
    static const PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestAlpha,
        /*  9 */ PipelineBlendFactor::kInvDestAlpha,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // While it's okay to specify fewer render targets in the pipeline state
    // (even fewer than written by the shader) than actually bound to the
    // command list (though this kind of truncation may only happen at the end -
    // DXGI_FORMAT_UNKNOWN *requires* a null RTV descriptor to be bound), not
    // doing that because sample counts of all render targets bound via
    // OMSetRenderTargets, even those beyond NumRenderTargets, apparently must
    // have their sample count matching the one set in the pipeline - however if
    // we set NumRenderTargets to 0 and also disable depth / stencil, the sample
    // count must be set to 1 - while the command list may still have
    // multisampled render targets bound (happens in 4D5307E6 main menu).
    // TODO(Triang3l): Investigate interaction of OMSetRenderTargets with
    // non-null depth and DSVFormat DXGI_FORMAT_UNKNOWN in the same case.
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i)))) {
        continue;
      }
      PipelineRenderTarget& rt = description_out.render_targets[i];
      rt.used = 1;
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      rt.format =
          xenos::ColorRenderTargetFormat(bound_depth_and_color_render_target_formats[1 + i]);
      rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
      if (rt.write_mask) {
        auto blendcontrol =
            regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
        rt.src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend)];
        rt.dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend)];
        rt.blend_op = blendcontrol.color_comb_fcn;
        rt.src_blend_alpha = kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend)];
        rt.dest_blend_alpha = kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend)];
        rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
      } else {
        rt.src_blend = PipelineBlendFactor::kOne;
        rt.dest_blend = PipelineBlendFactor::kZero;
        rt.blend_op = xenos::BlendOp::kAdd;
        rt.src_blend_alpha = PipelineBlendFactor::kOne;
        rt.dest_blend_alpha = PipelineBlendFactor::kZero;
        rt.blend_op_alpha = xenos::BlendOp::kAdd;
      }
    }
  }
  xenos::MsaaSamples host_msaa_samples = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  if (edram_rov_used) {
    if (host_msaa_samples == xenos::MsaaSamples::k2X) {
      // 2 is not supported in ForcedSampleCount on Nvidia.
      host_msaa_samples = xenos::MsaaSamples::k4X;
    }
  } else {
    if (!(bound_depth_and_color_render_target_bits & ~uint32_t(1)) &&
        !depth_stencil_bound_and_used) {
      // Direct3D 12 requires the sample count to be 1 when no color or depth /
      // stencil render targets are bound.
      // FIXME(Triang3l): Use ForcedSampleCount or some other fallback for
      // sample counting when needed, though with 2x it will be as incorrect as
      // with 1x / 4x anyway; or bind a dummy depth / stencil buffer if really
      // needed.
      host_msaa_samples = xenos::MsaaSamples::k1X;
    }
    // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported.
  }
  description_out.host_msaa_samples = host_msaa_samples;

  return true;
}

bool PipelineCache::GetGeometryShaderKey(
    PipelineGeometryShader geometry_shader_type,
    DxbcShaderTranslator::Modification vertex_shader_modification,
    DxbcShaderTranslator::Modification pixel_shader_modification, GeometryShaderKey& key_out) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return false;
  }
  assert_true(vertex_shader_modification.vertex.interpolator_mask ==
              pixel_shader_modification.pixel.interpolator_mask);
  GeometryShaderKey key;
  key.type = geometry_shader_type;
  key.interpolator_count = rex::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  key_out = key;
  return true;
}

void PipelineCache::CreateDxbcGeometryShader(GeometryShaderKey key,
                                             std::vector<uint32_t>& shader_out) {
  shader_out.clear();
  uint32_t point_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t point_user_cull_distance_count =
      key.type == PipelineGeometryShader::kPointList && key.user_clip_plane_cull
          ? key.user_clip_plane_count
          : 0;
  bool point_recalculate_clip_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_clip_distance_count && key.point_ps_ucp_mode >= 2;
  bool point_recalculate_cull_distances = key.type == PipelineGeometryShader::kPointList &&
                                          point_user_cull_distance_count &&
                                          key.point_ps_ucp_mode >= 3;

  // RDEF, ISGN, OSG5, SHEX, STAT.
  constexpr uint32_t kBlobCount = 5;

  // Allocate space for the container header and the blob offsets.
  shader_out.resize(sizeof(dxbc::ContainerHeader) / sizeof(uint32_t) + kBlobCount);
  uint32_t blob_offset_position_dwords = sizeof(dxbc::ContainerHeader) / sizeof(uint32_t);
  uint32_t blob_position_dwords = uint32_t(shader_out.size());
  constexpr uint32_t kBlobHeaderSizeDwords = sizeof(dxbc::BlobHeader) / sizeof(uint32_t);

  uint32_t name_ptr;

  // ***************************************************************************
  // Resource definition
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t rdef_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  // Not needed, as the next operation done is resize, to allocate the space for
  // both the blob header and the resource definition header.
  // shader_out.resize(rdef_position_dwords);

  // RDEF header - the actual definitions will be written if needed.
  shader_out.resize(rdef_position_dwords + sizeof(dxbc::RdefHeader) / sizeof(uint32_t));
  // Generator name.
  dxbc::AppendAlignedString(shader_out, "Xenia");
  {
    auto& rdef_header =
        *reinterpret_cast<dxbc::RdefHeader*>(shader_out.data() + rdef_position_dwords);
    rdef_header.shader_model = dxbc::RdefShaderModel::kGeometryShader5_1;
    rdef_header.compile_flags =
        dxbc::kCompileFlagNoPreshader | dxbc::kCompileFlagPreferFlowControl |
        dxbc::kCompileFlagIeeeStrictness | dxbc::kCompileFlagAllResourcesBound;
    // Generator name is right after the header.
    rdef_header.generator_name_ptr = sizeof(dxbc::RdefHeader);
    rdef_header.fourcc = dxbc::RdefHeader::FourCC::k5_1;
    rdef_header.InitializeSizes();
  }

  uint32_t system_cbuffer_size_vector_aligned_bytes = 0;

  if (key.type == PipelineGeometryShader::kPointList) {
    // Need point parameters from the system constants.

    // Constant types - float2 only.
    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_float2 = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "float2");
    // Types.
    uint32_t rdef_type_float2_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_type_float2_ptr =
        uint32_t((rdef_type_float2_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    shader_out.resize(rdef_type_float2_position_dwords + sizeof(dxbc::RdefType) / sizeof(uint32_t));
    {
      auto& rdef_type_float2 =
          *reinterpret_cast<dxbc::RdefType*>(shader_out.data() + rdef_type_float2_position_dwords);
      rdef_type_float2.variable_class = dxbc::RdefVariableClass::kVector;
      rdef_type_float2.variable_type = dxbc::RdefVariableType::kFloat;
      rdef_type_float2.row_count = 1;
      rdef_type_float2.column_count = 2;
      rdef_type_float2.name_ptr = rdef_name_ptr_float2;
    }

    // Constants:
    // - float2 xe_point_constant_diameter
    // - float2 xe_point_screen_diameter_to_ndc_radius
    enum PointConstant : uint32_t {
      kPointConstantConstantDiameter,
      kPointConstantScreenDiameterToNDCRadius,
      kPointConstantCount,
    };
    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_point_constant_diameter = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_constant_diameter");
    uint32_t rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_point_screen_diameter_to_ndc_radius");
    // Constants.
    uint32_t rdef_constants_position_dwords = uint32_t(shader_out.size());
    uint32_t rdef_constants_ptr =
        uint32_t((rdef_constants_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    shader_out.resize(rdef_constants_position_dwords +
                      sizeof(dxbc::RdefVariable) / sizeof(uint32_t) * kPointConstantCount);
    {
      auto rdef_constants =
          reinterpret_cast<dxbc::RdefVariable*>(shader_out.data() + rdef_constants_position_dwords);
      // float2 xe_point_constant_diameter
      static_assert(sizeof(DxbcShaderTranslator::SystemConstants ::point_constant_diameter) ==
                        sizeof(float) * 2,
                    "DxbcShaderTranslator point_constant_diameter system constant size "
                    "differs between the shader translator and geometry shader "
                    "generation");
      static_assert_size(DxbcShaderTranslator::SystemConstants::point_constant_diameter,
                         sizeof(float) * 2);
      dxbc::RdefVariable& rdef_constant_point_constant_diameter =
          rdef_constants[kPointConstantConstantDiameter];
      rdef_constant_point_constant_diameter.name_ptr = rdef_name_ptr_xe_point_constant_diameter;
      rdef_constant_point_constant_diameter.start_offset_bytes =
          offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter);
      rdef_constant_point_constant_diameter.size_bytes = sizeof(float) * 2;
      rdef_constant_point_constant_diameter.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_constant_diameter.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_constant_diameter.start_texture = UINT32_MAX;
      rdef_constant_point_constant_diameter.start_sampler = UINT32_MAX;
      // float2 xe_point_screen_diameter_to_ndc_radius
      static_assert(
          sizeof(DxbcShaderTranslator::SystemConstants ::point_screen_diameter_to_ndc_radius) ==
              sizeof(float) * 2,
          "DxbcShaderTranslator point_screen_diameter_to_ndc_radius system "
          "constant size differs between the shader translator and geometry "
          "shader generation");
      dxbc::RdefVariable& rdef_constant_point_screen_diameter_to_ndc_radius =
          rdef_constants[kPointConstantScreenDiameterToNDCRadius];
      rdef_constant_point_screen_diameter_to_ndc_radius.name_ptr =
          rdef_name_ptr_xe_point_screen_diameter_to_ndc_radius;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_offset_bytes =
          offsetof(DxbcShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius);
      rdef_constant_point_screen_diameter_to_ndc_radius.size_bytes = sizeof(float) * 2;
      rdef_constant_point_screen_diameter_to_ndc_radius.flags = dxbc::kRdefVariableFlagUsed;
      rdef_constant_point_screen_diameter_to_ndc_radius.type_ptr = rdef_type_float2_ptr;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_texture = UINT32_MAX;
      rdef_constant_point_screen_diameter_to_ndc_radius.start_sampler = UINT32_MAX;
    }

    // Constant buffers - xe_system_cbuffer only.

    // Names.
    name_ptr = uint32_t((shader_out.size() - rdef_position_dwords) * sizeof(uint32_t));
    uint32_t rdef_name_ptr_xe_system_cbuffer = name_ptr;
    name_ptr += dxbc::AppendAlignedString(shader_out, "xe_system_cbuffer");
    // Constant buffers.
    uint32_t rdef_cbuffer_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_cbuffer_position_dwords + sizeof(dxbc::RdefCbuffer) / sizeof(uint32_t));
    {
      auto& rdef_cbuffer_system =
          *reinterpret_cast<dxbc::RdefCbuffer*>(shader_out.data() + rdef_cbuffer_position_dwords);
      rdef_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_cbuffer_system.variable_count = kPointConstantCount;
      rdef_cbuffer_system.variables_ptr = rdef_constants_ptr;
      auto rdef_constants = reinterpret_cast<const dxbc::RdefVariable*>(
          shader_out.data() + rdef_constants_position_dwords);
      for (uint32_t i = 0; i < kPointConstantCount; ++i) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     rdef_constants[i].start_offset_bytes + rdef_constants[i].size_bytes);
      }
      if (point_recalculate_clip_distances || point_recalculate_cull_distances) {
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) +
                              sizeof(float) * 3));
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) +
                              sizeof(float) * 3));
        system_cbuffer_size_vector_aligned_bytes =
            std::max(system_cbuffer_size_vector_aligned_bytes,
                     uint32_t(offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                              sizeof(float) * 4 * 6));
      }
      system_cbuffer_size_vector_aligned_bytes =
          rex::align(system_cbuffer_size_vector_aligned_bytes, uint32_t(sizeof(uint32_t) * 4));
      rdef_cbuffer_system.size_vector_aligned_bytes = system_cbuffer_size_vector_aligned_bytes;
    }

    // Bindings - xe_system_cbuffer only.
    uint32_t rdef_binding_position_dwords = uint32_t(shader_out.size());
    shader_out.resize(rdef_binding_position_dwords +
                      sizeof(dxbc::RdefInputBind) / sizeof(uint32_t));
    {
      auto& rdef_binding_cbuffer_system =
          *reinterpret_cast<dxbc::RdefInputBind*>(shader_out.data() + rdef_binding_position_dwords);
      rdef_binding_cbuffer_system.name_ptr = rdef_name_ptr_xe_system_cbuffer;
      rdef_binding_cbuffer_system.type = dxbc::RdefInputType::kCbuffer;
      rdef_binding_cbuffer_system.bind_point =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      rdef_binding_cbuffer_system.bind_count = 1;
      rdef_binding_cbuffer_system.flags = dxbc::kRdefInputFlagUserPacked;
    }

    // Pointers in the header.
    {
      auto& rdef_header =
          *reinterpret_cast<dxbc::RdefHeader*>(shader_out.data() + rdef_position_dwords);
      rdef_header.cbuffer_count = 1;
      rdef_header.cbuffers_ptr =
          uint32_t((rdef_cbuffer_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
      rdef_header.input_bind_count = 1;
      rdef_header.input_binds_ptr =
          uint32_t((rdef_binding_position_dwords - rdef_position_dwords) * sizeof(uint32_t));
    }
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kResourceDefinition;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Input signature
  // ***************************************************************************

  // Clip and cull distances are tightly packed together into registers, but
  // have separate signature parameters with each being a vec4-aligned window.
  uint32_t input_clip_distance_count = key.user_clip_plane_cull ? 0 : key.user_clip_plane_count;
  uint32_t input_cull_distance_count =
      (key.user_clip_plane_cull ? key.user_clip_plane_count : 0) + key.has_vertex_kill_and;
  uint32_t input_clip_and_cull_distance_count =
      input_clip_distance_count + input_cull_distance_count;

  // Interpolators, position, clip and cull distances (parameters containing
  // only clip or cull distances, and also one parameter containing both if
  // present), point size.
  uint32_t isgn_parameter_count =
      key.interpolator_count + 1 + ((input_clip_and_cull_distance_count + 3) / 4) +
      uint32_t(input_cull_distance_count && (input_clip_distance_count & 3) != 0) +
      key.has_point_size;

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t isgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameter) / sizeof(uint32_t) * isgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((shader_out.size() - isgn_position_dwords) * sizeof(uint32_t));
  uint32_t isgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t isgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t isgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }
  uint32_t isgn_name_ptr_sv_cull_distance = name_ptr;
  if (input_cull_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_CullDistance");
  }
  uint32_t isgn_name_ptr_xepsize = name_ptr;
  if (key.has_point_size) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XEPSIZE");
  }

  // Header and parameters.
  uint32_t input_register_interpolators = UINT32_MAX;
  uint32_t input_register_position;
  uint32_t input_register_clip_and_cull_distances = UINT32_MAX;
  uint32_t input_register_point_size = UINT32_MAX;
  {
    // Header.
    auto& isgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + isgn_position_dwords);
    isgn_header.parameter_count = isgn_parameter_count;
    isgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto isgn_parameters = reinterpret_cast<dxbc::SignatureParameter*>(
        shader_out.data() + isgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t isgn_parameter_index = 0;
    uint32_t input_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      input_register_interpolators = input_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(isgn_parameter_index < isgn_parameter_count);
        dxbc::SignatureParameter& isgn_interpolator = isgn_parameters[isgn_parameter_index++];
        isgn_interpolator.semantic_name_ptr = isgn_name_ptr_texcoord;
        isgn_interpolator.semantic_index = i;
        isgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        isgn_interpolator.register_index = input_register_index++;
        isgn_interpolator.mask = 0b1111;
        isgn_interpolator.always_reads_mask = 0b1111;
      }
    }

    // Position (SV_Position).
    input_register_position = input_register_index;
    assert_true(isgn_parameter_index < isgn_parameter_count);
    dxbc::SignatureParameter& isgn_sv_position = isgn_parameters[isgn_parameter_index++];
    isgn_sv_position.semantic_name_ptr = isgn_name_ptr_sv_position;
    isgn_sv_position.system_value = dxbc::Name::kPosition;
    isgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    isgn_sv_position.register_index = input_register_index++;
    isgn_sv_position.mask = 0b1111;
    isgn_sv_position.always_reads_mask = 0b1111;

    // Clip and cull distances (SV_ClipDistance#, SV_CullDistance#).
    if (input_clip_and_cull_distance_count) {
      input_register_clip_and_cull_distances = input_register_index;
      uint32_t isgn_cull_distance_semantic_index = 0;
      for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
        if (i < input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_clip_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_clip_distance.semantic_name_ptr = isgn_name_ptr_sv_clip_distance;
          isgn_sv_clip_distance.semantic_index = i / 4;
          isgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
          isgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_clip_distance.register_index = input_register_index;
          uint8_t isgn_sv_clip_distance_mask =
              (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
          isgn_sv_clip_distance.mask = isgn_sv_clip_distance_mask;
          isgn_sv_clip_distance.always_reads_mask = isgn_sv_clip_distance_mask;
        }
        if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
          dxbc::SignatureParameter& isgn_sv_cull_distance = isgn_parameters[isgn_parameter_index++];
          isgn_sv_cull_distance.semantic_name_ptr = isgn_name_ptr_sv_cull_distance;
          isgn_sv_cull_distance.semantic_index = isgn_cull_distance_semantic_index++;
          isgn_sv_cull_distance.system_value = dxbc::Name::kCullDistance;
          isgn_sv_cull_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
          isgn_sv_cull_distance.register_index = input_register_index;
          uint8_t isgn_sv_cull_distance_mask =
              (UINT8_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
          if (i < input_clip_distance_count) {
            isgn_sv_cull_distance_mask &= ~((UINT8_C(1) << (input_clip_distance_count - i)) - 1);
          }
          isgn_sv_cull_distance.mask = isgn_sv_cull_distance_mask;
          isgn_sv_cull_distance.always_reads_mask = isgn_sv_cull_distance_mask;
        }
        ++input_register_index;
      }
    }

    // Point size (XEPSIZE).
    if (key.has_point_size) {
      input_register_point_size = input_register_index;
      assert_true(isgn_parameter_index < isgn_parameter_count);
      dxbc::SignatureParameter& isgn_point_size = isgn_parameters[isgn_parameter_index++];
      isgn_point_size.semantic_name_ptr = isgn_name_ptr_xepsize;
      isgn_point_size.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      isgn_point_size.register_index = input_register_index++;
      isgn_point_size.mask = 0b0001;
      isgn_point_size.always_reads_mask =
          key.type == PipelineGeometryShader::kPointList ? 0b0001 : 0;
    }

    assert_true(isgn_parameter_index == isgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kInputSignature;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Output signature
  // ***************************************************************************

  // Interpolators, point coordinates, position, clip distances.
  uint32_t osgn_parameter_count = key.interpolator_count + key.has_point_coordinates + 1 +
                                  ((input_clip_distance_count + 3) / 4);

  // Reserve space for the header and the parameters.
  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t osgn_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t) +
                    sizeof(dxbc::SignatureParameterForGS) / sizeof(uint32_t) *
                        osgn_parameter_count);

  // Names (after the parameters).
  name_ptr = uint32_t((shader_out.size() - osgn_position_dwords) * sizeof(uint32_t));
  uint32_t osgn_name_ptr_texcoord = name_ptr;
  if (key.interpolator_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "TEXCOORD");
  }
  uint32_t osgn_name_ptr_xespritetexcoord = name_ptr;
  if (key.has_point_coordinates) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "XESPRITETEXCOORD");
  }
  uint32_t osgn_name_ptr_sv_position = name_ptr;
  name_ptr += dxbc::AppendAlignedString(shader_out, "SV_Position");
  uint32_t osgn_name_ptr_sv_clip_distance = name_ptr;
  if (input_clip_distance_count) {
    name_ptr += dxbc::AppendAlignedString(shader_out, "SV_ClipDistance");
  }

  // Header and parameters.
  uint32_t output_register_interpolators = UINT32_MAX;
  uint32_t output_register_point_coordinates = UINT32_MAX;
  uint32_t output_register_position;
  uint32_t output_register_clip_distances = UINT32_MAX;
  {
    // Header.
    auto& osgn_header =
        *reinterpret_cast<dxbc::Signature*>(shader_out.data() + osgn_position_dwords);
    osgn_header.parameter_count = osgn_parameter_count;
    osgn_header.parameter_info_ptr = sizeof(dxbc::Signature);

    // Parameters.
    auto osgn_parameters = reinterpret_cast<dxbc::SignatureParameterForGS*>(
        shader_out.data() + osgn_position_dwords + sizeof(dxbc::Signature) / sizeof(uint32_t));
    uint32_t osgn_parameter_index = 0;
    uint32_t output_register_index = 0;

    // Interpolators (TEXCOORD#).
    if (key.interpolator_count) {
      output_register_interpolators = output_register_index;
      for (uint32_t i = 0; i < key.interpolator_count; ++i) {
        assert_true(osgn_parameter_index < osgn_parameter_count);
        dxbc::SignatureParameterForGS& osgn_interpolator = osgn_parameters[osgn_parameter_index++];
        osgn_interpolator.semantic_name_ptr = osgn_name_ptr_texcoord;
        osgn_interpolator.semantic_index = i;
        osgn_interpolator.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_interpolator.register_index = output_register_index++;
        osgn_interpolator.mask = 0b1111;
      }
    }

    // Point coordinates (XESPRITETEXCOORD).
    if (key.has_point_coordinates) {
      output_register_point_coordinates = output_register_index;
      assert_true(osgn_parameter_index < osgn_parameter_count);
      dxbc::SignatureParameterForGS& osgn_point_coordinates =
          osgn_parameters[osgn_parameter_index++];
      osgn_point_coordinates.semantic_name_ptr = osgn_name_ptr_xespritetexcoord;
      osgn_point_coordinates.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
      osgn_point_coordinates.register_index = output_register_index++;
      osgn_point_coordinates.mask = 0b0011;
      osgn_point_coordinates.never_writes_mask = 0b1100;
    }

    // Position (SV_Position).
    output_register_position = output_register_index;
    assert_true(osgn_parameter_index < osgn_parameter_count);
    dxbc::SignatureParameterForGS& osgn_sv_position = osgn_parameters[osgn_parameter_index++];
    osgn_sv_position.semantic_name_ptr = osgn_name_ptr_sv_position;
    osgn_sv_position.system_value = dxbc::Name::kPosition;
    osgn_sv_position.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
    osgn_sv_position.register_index = output_register_index++;
    osgn_sv_position.mask = 0b1111;

    // Clip distances (SV_ClipDistance#).
    if (input_clip_distance_count) {
      output_register_clip_distances = output_register_index;
      for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
        dxbc::SignatureParameterForGS& osgn_sv_clip_distance =
            osgn_parameters[osgn_parameter_index++];
        osgn_sv_clip_distance.semantic_name_ptr = osgn_name_ptr_sv_clip_distance;
        osgn_sv_clip_distance.semantic_index = i / 4;
        osgn_sv_clip_distance.system_value = dxbc::Name::kClipDistance;
        osgn_sv_clip_distance.component_type = dxbc::SignatureRegisterComponentType::kFloat32;
        osgn_sv_clip_distance.register_index = output_register_index++;
        uint8_t osgn_sv_clip_distance_mask =
            (UINT8_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1;
        osgn_sv_clip_distance.mask = osgn_sv_clip_distance_mask;
        osgn_sv_clip_distance.never_writes_mask = osgn_sv_clip_distance_mask ^ 0b1111;
      }
    }

    assert_true(osgn_parameter_index == osgn_parameter_count);
  }

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kOutputSignatureForGS;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Shader program
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t shex_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(shex_position_dwords);

  shader_out.push_back(dxbc::VersionToken(dxbc::ProgramType::kGeometryShader, 5, 1));
  // Reserve space for the length token.
  shader_out.push_back(0);

  dxbc::Statistics stat;
  std::memset(&stat, 0, sizeof(dxbc::Statistics));
  dxbc::Assembler a(shader_out, stat);

  a.OpDclGlobalFlags(dxbc::kGlobalFlagAllResourcesBound);

  if (system_cbuffer_size_vector_aligned_bytes) {
    a.OpDclConstantBuffer(
        dxbc::Src::CB(dxbc::Src::Dcl, 0,
                      uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                      uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants)),
        system_cbuffer_size_vector_aligned_bytes / (sizeof(uint32_t) * 4));
  }

  dxbc::Primitive input_primitive = dxbc::Primitive::kUndefined;
  uint32_t input_primitive_vertex_count = 0;
  dxbc::PrimitiveTopology output_primitive_topology = dxbc::PrimitiveTopology::kUndefined;
  uint32_t max_output_vertex_count = 0;
  switch (key.type) {
    case PipelineGeometryShader::kPointList:
      // Point to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kPoint;
      input_primitive_vertex_count = 1;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kRectangleList:
      // Triangle to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kTriangle;
      input_primitive_vertex_count = 3;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    case PipelineGeometryShader::kQuadList:
      // 4 vertices passed via kLineWithAdjacency to a strip of 2 triangles.
      input_primitive = dxbc::Primitive::kLineWithAdjacency;
      input_primitive_vertex_count = 4;
      output_primitive_topology = dxbc::PrimitiveTopology::kTriangleStrip;
      max_output_vertex_count = 4;
      break;
    default:
      assert_unhandled_case(key.type);
  }

  assert_false(key.interpolator_count && input_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_interpolators + i));
  }
  a.OpDclInputSIV(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_position),
                  dxbc::Name::kPosition);
  // Clip and cull plane declarations are separate in FXC-generated code even
  // for a single register.
  assert_false(input_clip_and_cull_distance_count &&
               input_register_clip_and_cull_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_and_cull_distance_count; i += 4) {
    if (i < input_clip_distance_count) {
      a.OpDclInput(dxbc::Dest::V2D(
          input_primitive_vertex_count, input_register_clip_and_cull_distances + (i >> 2),
          (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1));
    }
    if (input_cull_distance_count && i + 4 > input_clip_distance_count) {
      uint32_t cull_distance_mask =
          (UINT32_C(1) << std::min(input_clip_and_cull_distance_count - i, UINT32_C(4))) - 1;
      if (i < input_clip_distance_count) {
        cull_distance_mask &= ~((UINT32_C(1) << (input_clip_distance_count - i)) - 1);
      }
      a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count,
                                   input_register_clip_and_cull_distances + (i >> 2),
                                   cull_distance_mask));
    }
  }
  if (key.has_point_size && key.type == PipelineGeometryShader::kPointList) {
    assert_true(input_register_point_size != UINT32_MAX);
    a.OpDclInput(dxbc::Dest::V2D(input_primitive_vertex_count, input_register_point_size, 0b0001));
  }

  // At least 1 temporary register needed to discard primitives with NaN
  // position.
  size_t dcl_temps_count_position_dwords = a.OpDclTemps(1);

  a.OpDclInputPrimitive(input_primitive);
  dxbc::Dest stream(dxbc::Dest::M(0));
  a.OpDclStream(stream);
  a.OpDclOutputTopology(output_primitive_topology);

  assert_false(key.interpolator_count && output_register_interpolators == UINT32_MAX);
  for (uint32_t i = 0; i < key.interpolator_count; ++i) {
    a.OpDclOutput(dxbc::Dest::O(output_register_interpolators + i));
  }
  if (key.has_point_coordinates) {
    assert_true(output_register_point_coordinates != UINT32_MAX);
    a.OpDclOutput(dxbc::Dest::O(output_register_point_coordinates, 0b0011));
  }
  a.OpDclOutputSIV(dxbc::Dest::O(output_register_position), dxbc::Name::kPosition);
  assert_false(input_clip_distance_count && output_register_clip_distances == UINT32_MAX);
  for (uint32_t i = 0; i < input_clip_distance_count; i += 4) {
    a.OpDclOutputSIV(
        dxbc::Dest::O(output_register_clip_distances + (i >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - i, UINT32_C(4))) - 1),
        dxbc::Name::kClipDistance);
  }

  a.OpDclMaxOutputVertexCount(max_output_vertex_count);

  // Note that after every emit, all o# become initialized and must be written
  // to again.
  // Also, FXC generates only movs (from statically or dynamically indexed
  // v[#][#], from r#, or from a literal) to o# for some reason.
  // emit_then_cut_stream must not be used - it crashes the shader compiler of
  // AMD Software: Adrenalin Edition 23.3.2 on RDNA 3 if it's conditional (after
  // a `retc` or inside an `if`), and it doesn't seem to be generated by FXC or
  // DXC at all.

  // Discard the whole primitive if any vertex has a NaN position (may also be
  // set to NaN for emulation of vertex killing with the OR operator).
  for (uint32_t i = 0; i < input_primitive_vertex_count; ++i) {
    a.OpNE(dxbc::Dest::R(0), dxbc::Src::V2D(i, input_register_position),
           dxbc::Src::V2D(i, input_register_position));
    a.OpOr(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b1110));
    a.OpOr(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
           dxbc::Src::R(0, dxbc::Src::kYYYY));
    a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
  }

  // Cull the whole primitive if any cull distance for all vertices in the
  // primitive is < 0.
  // For point lists with ps_ucp_mode 3, user cull plane distances are
  // calculated per expanded vertex later.
  if (input_cull_distance_count) {
    uint32_t cull_distance_start =
        point_recalculate_cull_distances ? point_user_cull_distance_count : 0;
    for (uint32_t i = cull_distance_start; i < input_cull_distance_count; ++i) {
      uint32_t cull_distance_register =
          input_register_clip_and_cull_distances + ((input_clip_distance_count + i) >> 2);
      uint32_t cull_distance_component = (input_clip_distance_count + i) & 3;
      a.OpLT(dxbc::Dest::R(0, 0b0001),
             dxbc::Src::V2D(0, cull_distance_register).Select(cull_distance_component),
             dxbc::Src::LF(0.0f));
      for (uint32_t j = 1; j < input_primitive_vertex_count; ++j) {
        a.OpLT(dxbc::Dest::R(0, 0b0010),
               dxbc::Src::V2D(j, cull_distance_register).Select(cull_distance_component),
               dxbc::Src::LF(0.0f));
        a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kXXXX),
                dxbc::Src::R(0, dxbc::Src::kYYYY));
      }
      a.OpRetC(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
    }
  }

  switch (key.type) {
    case PipelineGeometryShader::kPointList: {
      // Expand the point sprite, with left-to-right, top-to-bottom UVs.
      dxbc::Src point_size_src(dxbc::Src::CB(
          0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
          offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter) >> 4,
          ((offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter[0]) >> 2) & 3) |
              (((offsetof(DxbcShaderTranslator::SystemConstants, point_constant_diameter[1]) >> 2) &
                3)
               << 2)));
      if (key.has_point_size) {
        // The vertex shader's header writes -1.0 to point_size by default, so
        // any non-negative value means that it was overwritten by the
        // translated vertex shader, and needs to be used instead of the
        // constant size. The per-vertex diameter is already clamped in the
        // vertex shader (combined with making it non-negative).
        a.OpGE(dxbc::Dest::R(0, 0b0001),
               dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), dxbc::Src::LF(0.0f));
        a.OpMovC(dxbc::Dest::R(0, 0b0011), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::V2D(0, input_register_point_size, dxbc::Src::kXXXX), point_size_src);
        point_size_src = dxbc::Src::R(0, 0b0100);
      }
      // 4D5307F1 has zero-size snowflakes, drop them quicker, and also drop
      // points with a constant size of zero since point lists may also be used
      // as just "compute" with memexport.
      // XY may contain the point size with the per-vertex override applied, use
      // Z as temporary.
      for (uint32_t i = 0; i < 2; ++i) {
        a.OpLT(dxbc::Dest::R(0, 0b0100), dxbc::Src::LF(0.0f), point_size_src.SelectFromSwizzled(i));
        a.OpRetC(false, dxbc::Src::R(0, dxbc::Src::kZZZZ));
      }
      // Transform the diameter in the guest screen coordinates to radius in the
      // normalized device coordinates, and then to the clip space by
      // multiplying by W.
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                            offsetof(DxbcShaderTranslator::SystemConstants,
                                     point_screen_diameter_to_ndc_radius) >>
                                4,
                            ((offsetof(DxbcShaderTranslator::SystemConstants,
                                       point_screen_diameter_to_ndc_radius[0]) >>
                              2) &
                             3) |
                                (((offsetof(DxbcShaderTranslator::SystemConstants,
                                            point_screen_diameter_to_ndc_radius[1]) >>
                                   2) &
                                  3)
                                 << 2)));
      point_size_src = dxbc::Src::R(0, 0b0100);
      a.OpMul(dxbc::Dest::R(0, 0b0011), point_size_src,
              dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
      dxbc::Src point_radius_x_src(point_size_src.SelectFromSwizzled(0));
      dxbc::Src point_radius_y_src(point_size_src.SelectFromSwizzled(1));

      if (point_recalculate_cull_distances) {
        stat.temp_register_count = std::max(UINT32_C(4), stat.temp_register_count);
        dxbc::Src ndc_scale_xy(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4,
            ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3) << 2)));
        dxbc::Src ndc_scale_z(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
        dxbc::Src ndc_offset_xy(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4,
            ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                 << 2)));
        dxbc::Src ndc_offset_z(dxbc::Src::CB(
            0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
            offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
        for (uint32_t j = 0; j < point_user_cull_distance_count; ++j) {
          for (uint32_t i = 0; i < 4; ++i) {
            a.OpAdd(dxbc::Dest::R(2, 0b0001),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                    (i & 1) ? point_radius_x_src : -point_radius_x_src);
            a.OpAdd(dxbc::Dest::R(2, 0b0010),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                    (i >> 1) ? -point_radius_y_src : point_radius_y_src);
            a.OpMov(dxbc::Dest::R(2, 0b0100),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
            a.OpMov(dxbc::Dest::R(2, 0b1000),
                    dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
            a.OpMAd(dxbc::Dest::R(2, 0b0011), -ndc_offset_xy, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, 0b0011));
            a.OpDiv(dxbc::Dest::R(2, 0b0011), dxbc::Src::R(2, 0b0011), ndc_scale_xy);
            a.OpMAd(dxbc::Dest::R(2, 0b0100), -ndc_offset_z, dxbc::Src::R(2, dxbc::Src::kWWWW),
                    dxbc::Src::R(2, dxbc::Src::kZZZZ));
            a.OpDiv(dxbc::Dest::R(2, 0b0100), dxbc::Src::R(2, dxbc::Src::kZZZZ), ndc_scale_z);
            a.OpDP4(
                dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2),
                dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                              (offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                               sizeof(float) * 4 * j) >>
                                  4,
                              dxbc::Src::kXYZW));
            a.OpLT(dxbc::Dest::R(2, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX),
                   dxbc::Src::LF(0.0f));
            if (i == 0) {
              a.OpMov(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(2, dxbc::Src::kXXXX));
            } else {
              a.OpAnd(dxbc::Dest::R(3, 0b0001), dxbc::Src::R(3, dxbc::Src::kXXXX),
                      dxbc::Src::R(2, dxbc::Src::kXXXX));
            }
          }
          a.OpRetC(true, dxbc::Src::R(3, dxbc::Src::kXXXX));
        }
      }

      for (uint32_t i = 0; i < 4; ++i) {
        // Same interpolators for the entire sprite.
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(0, input_register_interpolators + j));
        }
        // Top-left, top-right, bottom-left, bottom-right order (chosen
        // arbitrarily, simply based on clockwise meaning front with
        // FrontCounterClockwise = FALSE, but faceness is ignored for
        // non-polygon primitive types).
        // Bottom is -Y in Direct3D NDC, +V in point sprite coordinates.
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011),
                  dxbc::Src::LF(float(i & 1), float(i >> 1), 0.0f, 0.0f));
        }
        // FXC generates only `mov`s for o#, use temporary registers (r0.zw, as
        // r0.xy already used for the point size) for calculations.
        a.OpAdd(dxbc::Dest::R(0, 0b0100),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kXXXX),
                (i & 1) ? point_radius_x_src : -point_radius_x_src);
        a.OpAdd(dxbc::Dest::R(0, 0b1000),
                dxbc::Src::V2D(0, input_register_position, dxbc::Src::kYYYY),
                (i >> 1) ? -point_radius_y_src : point_radius_y_src);
        a.OpMov(dxbc::Dest::O(output_register_position, 0b0011), dxbc::Src::R(0, 0b1110));
        a.OpMov(dxbc::Dest::O(output_register_position, 0b1100),
                dxbc::Src::V2D(0, input_register_position));
        if (point_recalculate_clip_distances) {
          // Convert host clip space back to guest clip space before applying
          // user clip planes.
          dxbc::Src ndc_scale_xy(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4,
              ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[0]) >> 2) & 3) |
                  (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_scale_z(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_scale) >> 4, dxbc::Src::kZZZZ));
          dxbc::Src ndc_offset_xy(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4,
              ((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[0]) >> 2) & 3) |
                  (((offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset[1]) >> 2) & 3)
                   << 2)));
          dxbc::Src ndc_offset_z(dxbc::Src::CB(
              0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
              offsetof(DxbcShaderTranslator::SystemConstants, ndc_offset) >> 4, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(0, 0b1110));
          a.OpMov(dxbc::Dest::R(1, 0b0100),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kZZZZ));
          a.OpMov(dxbc::Dest::R(1, 0b1000),
                  dxbc::Src::V2D(0, input_register_position, dxbc::Src::kWWWW));
          a.OpMAd(dxbc::Dest::R(1, 0b0011), -ndc_offset_xy, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, 0b0100));
          a.OpDiv(dxbc::Dest::R(1, 0b0011), dxbc::Src::R(1, 0b0100), ndc_scale_xy);
          a.OpMAd(dxbc::Dest::R(1, 0b0100), -ndc_offset_z, dxbc::Src::R(1, dxbc::Src::kWWWW),
                  dxbc::Src::R(1, dxbc::Src::kZZZZ));
          a.OpDiv(dxbc::Dest::R(1, 0b0100), dxbc::Src::R(1, dxbc::Src::kZZZZ), ndc_scale_z);
          for (uint32_t j = 0; j < input_clip_distance_count; ++j) {
            a.OpDP4(
                dxbc::Dest::O(output_register_clip_distances + (j >> 2), UINT32_C(1) << (j & 3)),
                dxbc::Src::R(1),
                dxbc::Src::CB(0, uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants),
                              (offsetof(DxbcShaderTranslator::SystemConstants, user_clip_planes) +
                               sizeof(float) * 4 * j) >>
                                  4,
                              dxbc::Src::kXYZW));
          }
        } else {
          for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
            a.OpMov(dxbc::Dest::O(
                        output_register_clip_distances + (j >> 2),
                        (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                    dxbc::Src::V2D(0, input_register_clip_and_cull_distances + (j >> 2)));
          }
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kRectangleList: {
      // Construct a strip with the fourth vertex generated by mirroring a
      // vertex across the longest edge (the diagonal).
      //
      // Possible options:
      //
      // 0---1
      // |  /|
      // | / |  - 12 is the longest edge, strip 0123 (most commonly used)
      // |/  |    v3 = v0 + (v1 - v0) + (v2 - v0), or v3 = -v0 + v1 + v2
      // 2--[3]
      //
      // 1---2
      // |  /|
      // | / |  - 20 is the longest edge, strip 1203
      // |/  |
      // 0--[3]
      //
      // 2---0
      // |  /|
      // | / |  - 01 is the longest edge, strip 2013
      // |/  |
      // 1--[3]
      //
      // Input vertices are implicitly indexable, dcl_indexRange is not needed
      // for the first dimension of a v[#][#] index.

      // Get squares of edge lengths into r0.xyz to choose the longest edge.
      // r0.x = ||12||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0011), dxbc::Src::V2D(2, input_register_position, 0b0100),
              -dxbc::Src::V2D(1, input_register_position, 0b0100));
      a.OpDP2(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, 0b0100), dxbc::Src::R(0, 0b0100));
      // r0.y = ||20||^2
      a.OpAdd(dxbc::Dest::R(0, 0b0110), dxbc::Src::V2D(0, input_register_position, 0b0100 << 2),
              -dxbc::Src::V2D(2, input_register_position, 0b0100 << 2));
      a.OpDP2(dxbc::Dest::R(0, 0b0010), dxbc::Src::R(0, 0b1001), dxbc::Src::R(0, 0b1001));
      // r0.z = ||01||^2
      a.OpAdd(dxbc::Dest::R(0, 0b1100), dxbc::Src::V2D(1, input_register_position, 0b0100 << 4),
              -dxbc::Src::V2D(0, input_register_position, 0b0100 << 4));
      a.OpDP2(dxbc::Dest::R(0, 0b0100), dxbc::Src::R(0, 0b1110), dxbc::Src::R(0, 0b1110));

      // Find the longest edge, and select the strip vertex indices into r0.xyz.
      // r0.w = 12 > 20
      a.OpLT(dxbc::Dest::R(0, 0b1000), dxbc::Src::R(0, dxbc::Src::kYYYY),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 01
      a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
             dxbc::Src::R(0, dxbc::Src::kXXXX));
      // r0.x = 12 > 20 && 12 > 01
      a.OpAnd(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kWWWW),
              dxbc::Src::R(0, dxbc::Src::kXXXX));
      a.OpIf(true, dxbc::Src::R(0, dxbc::Src::kXXXX));
      {
        // 12 is the longest edge, the first triangle in the strip is 012.
        a.OpMov(dxbc::Dest::R(0, 0b0111), dxbc::Src::LU(0, 1, 2, 0));
      }
      a.OpElse();
      {
        // r0.x = 20 > 01
        a.OpLT(dxbc::Dest::R(0, 0b0001), dxbc::Src::R(0, dxbc::Src::kZZZZ),
               dxbc::Src::R(0, dxbc::Src::kYYYY));
        // If 20 is the longest edge, the first triangle in the strip is 120.
        // Otherwise, it's 201.
        a.OpMovC(dxbc::Dest::R(0, 0b0111), dxbc::Src::R(0, dxbc::Src::kXXXX),
                 dxbc::Src::LU(1, 2, 0, 0), dxbc::Src::LU(2, 0, 1, 0));
      }
      a.OpEndIf();

      // Emit the triangle in the strip that consists of the original vertices.
      for (uint32_t i = 0; i < 3; ++i) {
        dxbc::Index input_vertex_index(0, i);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }

      // Construct the fourth vertex using r1 as temporary storage, including
      // for the final operation as FXC generates only `mov`s for o#.
      stat.temp_register_count = std::max(UINT32_C(2), stat.temp_register_count);
      for (uint32_t j = 0; j < key.interpolator_count; ++j) {
        uint32_t input_register_interpolator = input_register_interpolators + j;
        a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_interpolator),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_interpolator));
        a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_interpolator));
        a.OpMov(dxbc::Dest::O(output_register_interpolators + j), dxbc::Src::R(1));
      }
      if (key.has_point_coordinates) {
        a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
      }
      a.OpAdd(dxbc::Dest::R(1), -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_position),
              dxbc::Src::V2D(dxbc::Index(0, 1), input_register_position));
      a.OpAdd(dxbc::Dest::R(1), dxbc::Src::R(1),
              dxbc::Src::V2D(dxbc::Index(0, 2), input_register_position));
      a.OpMov(dxbc::Dest::O(output_register_position), dxbc::Src::R(1));
      for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
        uint32_t clip_distance_mask =
            (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1;
        uint32_t input_register_clip_distance = input_register_clip_and_cull_distances + (j >> 2);
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask),
                -dxbc::Src::V2D(dxbc::Index(0, 0), input_register_clip_distance),
                dxbc::Src::V2D(dxbc::Index(0, 1), input_register_clip_distance));
        a.OpAdd(dxbc::Dest::R(1, clip_distance_mask), dxbc::Src::R(1),
                dxbc::Src::V2D(dxbc::Index(0, 2), input_register_clip_distance));
        a.OpMov(dxbc::Dest::O(output_register_clip_distances + (j >> 2), clip_distance_mask),
                dxbc::Src::R(1));
      }
      a.OpEmitStream(stream);
      a.OpCutStream(stream);
    } break;

    case PipelineGeometryShader::kQuadList: {
      // Build the triangle strip from the original quad vertices in the
      // 0, 1, 3, 2 order (like specified for GL_QUAD_STRIP).
      // TODO(Triang3l): Find the correct decomposition of quads into triangles
      // on the real hardware.
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t input_vertex_index = i ^ (i >> 1);
        for (uint32_t j = 0; j < key.interpolator_count; ++j) {
          a.OpMov(dxbc::Dest::O(output_register_interpolators + j),
                  dxbc::Src::V2D(input_vertex_index, input_register_interpolators + j));
        }
        if (key.has_point_coordinates) {
          a.OpMov(dxbc::Dest::O(output_register_point_coordinates, 0b0011), dxbc::Src::LF(0.0f));
        }
        a.OpMov(dxbc::Dest::O(output_register_position),
                dxbc::Src::V2D(input_vertex_index, input_register_position));
        for (uint32_t j = 0; j < input_clip_distance_count; j += 4) {
          a.OpMov(dxbc::Dest::O(
                      output_register_clip_distances + (j >> 2),
                      (UINT32_C(1) << std::min(input_clip_distance_count - j, UINT32_C(4))) - 1),
                  dxbc::Src::V2D(input_vertex_index,
                                 input_register_clip_and_cull_distances + (j >> 2)));
        }
        a.OpEmitStream(stream);
      }
      a.OpCutStream(stream);
    } break;

    default:
      assert_unhandled_case(key.type);
  }

  a.OpRet();

  // Write the actual number of temporary registers used.
  shader_out[dcl_temps_count_position_dwords] = stat.temp_register_count;

  // Write the shader program length in dwords.
  shader_out[shex_position_dwords + 1] = uint32_t(shader_out.size()) - shex_position_dwords;

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kShaderEx;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Statistics
  // ***************************************************************************

  shader_out[blob_offset_position_dwords] = uint32_t(blob_position_dwords * sizeof(uint32_t));
  uint32_t stat_position_dwords = blob_position_dwords + kBlobHeaderSizeDwords;
  shader_out.resize(stat_position_dwords + sizeof(dxbc::Statistics) / sizeof(uint32_t));
  std::memcpy(shader_out.data() + stat_position_dwords, &stat, sizeof(dxbc::Statistics));

  {
    auto& blob_header =
        *reinterpret_cast<dxbc::BlobHeader*>(shader_out.data() + blob_position_dwords);
    blob_header.fourcc = dxbc::BlobHeader::FourCC::kStatistics;
    blob_position_dwords = uint32_t(shader_out.size());
    blob_header.size_bytes = (blob_position_dwords - kBlobHeaderSizeDwords) * sizeof(uint32_t) -
                             shader_out[blob_offset_position_dwords++];
  }

  // ***************************************************************************
  // Container header
  // ***************************************************************************

  uint32_t shader_size_bytes = uint32_t(shader_out.size() * sizeof(uint32_t));
  {
    auto& container_header = *reinterpret_cast<dxbc::ContainerHeader*>(shader_out.data());
    container_header.InitializeIdentification();
    container_header.size_bytes = shader_size_bytes;
    container_header.blob_count = kBlobCount;
    CalculateDXBCChecksum(reinterpret_cast<unsigned char*>(shader_out.data()),
                          static_cast<unsigned int>(shader_size_bytes),
                          reinterpret_cast<unsigned int*>(&container_header.hash));
  }
}

const std::vector<uint32_t>& PipelineCache::GetGeometryShader(GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  std::vector<uint32_t> shader;
  CreateDxbcGeometryShader(key, shader);
  return geometry_shaders_.emplace(key, std::move(shader)).first->second;
}

ID3D12PipelineState* PipelineCache::CreateD3D12Pipeline(
    const PipelineRuntimeDescription& runtime_description) {
  const PipelineDescription& description = runtime_description.description;

  if (runtime_description.pixel_shader != nullptr) {
    REXGPU_DEBUG("Creating graphics pipeline with VS {:016X}, PS {:016X}",
                 runtime_description.vertex_shader->shader().ucode_data_hash(),
                 runtime_description.pixel_shader->shader().ucode_data_hash());
  } else {
    REXGPU_DEBUG("Creating graphics pipeline with VS {:016X}",
                 runtime_description.vertex_shader->shader().ucode_data_hash());
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC state_desc;
  std::memset(&state_desc, 0, sizeof(state_desc));

  bool edram_rov_used =
      render_target_cache_.GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  state_desc.pRootSignature = runtime_description.root_signature;

  // Index buffer strip cut value.
  switch (description.strip_cut_index) {
    case PipelineStripCutIndex::kFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineStripCutIndex::kFFFFFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }

  // Primitive topology, vertex, hull, domain and geometry shaders.
  if (!runtime_description.vertex_shader->is_translated()) {
    REXGPU_ERROR("Vertex shader {:016X} not translated",
                 runtime_description.vertex_shader->shader().ucode_data_hash());
    assert_always();
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      DxbcShaderTranslator::Modification(runtime_description.vertex_shader->modification())
          .vertex.host_vertex_shader_type;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    xenos::TessellationMode tessellation_mode =
        xenos::TessellationMode(description.primitive_topology_type_or_tessellation_mode);
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      state_desc.VS.pShaderBytecode = shaders::tessellation_adaptive_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_adaptive_vs);
    } else {
      state_desc.VS.pShaderBytecode = shaders::tessellation_indexed_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_indexed_vs);
    }
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_3cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_1cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_4cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_1cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::discrete_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_3cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_1cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_4cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_1cp_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::continuous_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_triangle_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::adaptive_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      default:
        assert_unhandled_case(tessellation_mode);
        return nullptr;
    }
    state_desc.DS.pShaderBytecode = runtime_description.vertex_shader->translated_binary().data();
    state_desc.DS.BytecodeLength = runtime_description.vertex_shader->translated_binary().size();
  } else {
    assert_true(host_vertex_shader_type == Shader::HostVertexShaderType::kVertex);
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      // Fallback vertex shaders are not needed on Direct3D 12.
      return nullptr;
    }
    state_desc.VS.pShaderBytecode = runtime_description.vertex_shader->translated_binary().data();
    state_desc.VS.BytecodeLength = runtime_description.vertex_shader->translated_binary().size();
    PipelinePrimitiveTopologyType primitive_topology_type =
        PipelinePrimitiveTopologyType(description.primitive_topology_type_or_tessellation_mode);
    switch (primitive_topology_type) {
      case PipelinePrimitiveTopologyType::kPoint:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case PipelinePrimitiveTopologyType::kLine:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case PipelinePrimitiveTopologyType::kTriangle:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        assert_unhandled_case(primitive_topology_type);
        return nullptr;
    }
  }

  // Pixel shader.
  if (runtime_description.pixel_shader != nullptr) {
    if (!runtime_description.pixel_shader->is_translated()) {
      REXGPU_ERROR("Pixel shader {:016X} not translated",
                   runtime_description.pixel_shader->shader().ucode_data_hash());
      assert_always();
      return nullptr;
    }
    state_desc.PS.pShaderBytecode = runtime_description.pixel_shader->translated_binary().data();
    state_desc.PS.BytecodeLength = runtime_description.pixel_shader->translated_binary().size();
  } else if (edram_rov_used) {
    state_desc.PS.pShaderBytecode = depth_only_pixel_shader_.data();
    state_desc.PS.BytecodeLength = depth_only_pixel_shader_.size();
  } else {
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        (description.depth_func != xenos::CompareFunction::kAlways || description.depth_write) &&
        description.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      if (render_target_cache_.depth_float24_round()) {
        state_desc.PS.pShaderBytecode = shaders::float24_round_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_round_ps);
      } else {
        state_desc.PS.pShaderBytecode = shaders::float24_truncate_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_truncate_ps);
      }
    }
  }

  // Geometry shader.
  if (runtime_description.geometry_shader != nullptr) {
    state_desc.GS.pShaderBytecode = runtime_description.geometry_shader->data();
    state_desc.GS.BytecodeLength = sizeof(*runtime_description.geometry_shader->data()) *
                                   runtime_description.geometry_shader->size();
  }

  // Rasterizer state.
  state_desc.RasterizerState.FillMode =
      description.fill_mode_wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case PipelineCullMode::kBack:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      assert_true(description.cull_mode == PipelineCullMode::kNone ||
                  description.cull_mode == PipelineCullMode::kDisableRasterization);
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  state_desc.RasterizerState.FrontCounterClockwise =
      description.front_counter_clockwise ? TRUE : FALSE;
  state_desc.RasterizerState.DepthBias = description.depth_bias;
  state_desc.RasterizerState.DepthBiasClamp = 0.0f;
  // With non-square resolution scaling, make sure the worst-case impact is
  // reverted (slope only along the scaled axis), thus max. More bias is better
  // than less bias, because less bias means Z fighting with the background is
  // more likely.
  state_desc.RasterizerState.SlopeScaledDepthBias =
      description.depth_bias_slope_scaled *
      float(std::max(render_target_cache_.draw_resolution_scale_x(),
                     render_target_cache_.draw_resolution_scale_y()));
  state_desc.RasterizerState.DepthClipEnable = description.depth_clip ? TRUE : FALSE;
  uint32_t msaa_sample_count = uint32_t(1) << uint32_t(description.host_msaa_samples);
  if (edram_rov_used) {
    // Only 1, 4, 8 and (not on all GPUs) 16 are allowed, using sample 0 as 0
    // and 3 as 1 for 2x instead (not exactly the same sample positions, but
    // still top-left and bottom-right - however, this can be adjusted with
    // programmable sample positions).
    assert_true(msaa_sample_count == 1 || msaa_sample_count == 4);
    if (msaa_sample_count != 1 && msaa_sample_count != 4) {
      return nullptr;
    }
    state_desc.RasterizerState.ForcedSampleCount = uint32_t(1)
                                                   << uint32_t(description.host_msaa_samples);
  }

  // Sample mask and description.
  state_desc.SampleMask = UINT_MAX;
  // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported without ROV.
  if (edram_rov_used) {
    state_desc.SampleDesc.Count = 1;
  } else {
    assert_true(msaa_sample_count <= 4);
    if (msaa_sample_count > 4) {
      return nullptr;
    }
    if (msaa_sample_count == 2 && !render_target_cache_.msaa_2x_supported()) {
      // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
      // sample positions, but still top-left and bottom-right - however, this
      // can be adjusted with programmable sample positions).
      state_desc.SampleMask = 0b1001;
      state_desc.SampleDesc.Count = 4;
    } else {
      state_desc.SampleDesc.Count = msaa_sample_count;
    }
  }

  if (!edram_rov_used) {
    // Depth/stencil.
    if (description.depth_func != xenos::CompareFunction::kAlways || description.depth_write) {
      state_desc.DepthStencilState.DepthEnable = TRUE;
      state_desc.DepthStencilState.DepthWriteMask =
          description.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions are the same in Direct3D 12 but plus one (minus
      // one, bit 0 for less, bit 1 for equal, bit 2 for greater).
      state_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(
          uint32_t(D3D12_COMPARISON_FUNC_NEVER) + uint32_t(description.depth_func));
    }
    if (description.stencil_enable) {
      state_desc.DepthStencilState.StencilEnable = TRUE;
      state_desc.DepthStencilState.StencilReadMask = description.stencil_read_mask;
      state_desc.DepthStencilState.StencilWriteMask = description.stencil_write_mask;
      // Stencil operations are the same in Direct3D 12 too but plus one.
      state_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_depth_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_front_pass_op));
      state_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC(
          uint32_t(D3D12_COMPARISON_FUNC_NEVER) + uint32_t(description.stencil_front_func));
      state_desc.DepthStencilState.BackFace.StencilFailOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_fail_op));
      state_desc.DepthStencilState.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_depth_fail_op));
      state_desc.DepthStencilState.BackFace.StencilPassOp = D3D12_STENCIL_OP(
          uint32_t(D3D12_STENCIL_OP_KEEP) + uint32_t(description.stencil_back_pass_op));
      state_desc.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC(
          uint32_t(D3D12_COMPARISON_FUNC_NEVER) + uint32_t(description.stencil_back_func));
    }
    if (state_desc.DepthStencilState.DepthEnable || state_desc.DepthStencilState.StencilEnable) {
      state_desc.DSVFormat =
          D3D12RenderTargetCache::GetDepthDSVDXGIFormat(description.depth_format);
    }

    // Render targets and blending.
    state_desc.BlendState.IndependentBlendEnable = TRUE;
    static const D3D12_BLEND kBlendFactorMap[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT,
    };
    // 8 entries for safety since 3 bits from the guest are passed directly.
    static const D3D12_BLEND_OP kBlendOpMap[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,     D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const PipelineRenderTarget& rt = description.render_targets[i];
      if (!rt.used) {
        // Null RTV descriptors can be used for slots with DXGI_FORMAT_UNKNOWN
        // in the pipeline state.
        state_desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        continue;
      }
      state_desc.NumRenderTargets = i + 1;
      state_desc.RTVFormats[i] = render_target_cache_.GetColorDrawDXGIFormat(rt.format);
      if (state_desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) {
        assert_always();
        return nullptr;
      }
      D3D12_RENDER_TARGET_BLEND_DESC& blend_desc = state_desc.BlendState.RenderTarget[i];
      if (rt.src_blend != PipelineBlendFactor::kOne ||
          rt.dest_blend != PipelineBlendFactor::kZero || rt.blend_op != xenos::BlendOp::kAdd ||
          rt.src_blend_alpha != PipelineBlendFactor::kOne ||
          rt.dest_blend_alpha != PipelineBlendFactor::kZero ||
          rt.blend_op_alpha != xenos::BlendOp::kAdd) {
        blend_desc.BlendEnable = TRUE;
        blend_desc.SrcBlend = kBlendFactorMap[uint32_t(rt.src_blend)];
        blend_desc.DestBlend = kBlendFactorMap[uint32_t(rt.dest_blend)];
        blend_desc.BlendOp = kBlendOpMap[uint32_t(rt.blend_op)];
        blend_desc.SrcBlendAlpha = kBlendFactorMap[uint32_t(rt.src_blend_alpha)];
        blend_desc.DestBlendAlpha = kBlendFactorMap[uint32_t(rt.dest_blend_alpha)];
        blend_desc.BlendOpAlpha = kBlendOpMap[uint32_t(rt.blend_op_alpha)];
      }
      blend_desc.RenderTargetWriteMask = rt.write_mask;
    }
  }

  // Disable rasterization if needed (parameter combinations that make no
  // difference when rasterization is disabled have already been handled in
  // GetCurrentStateDescription) the way it's disabled in Direct3D by design
  // (disabling a pixel shader and depth / stencil).
  // TODO(Triang3l): When it happens to be that a combination of parameters
  // (no host pixel shader and depth / stencil without ROV) would disable
  // rasterization when it's still needed (for occlusion query sample counting),
  // ensure rasterization happens (by binding an empty pixel shader, or maybe
  // via ForcedSampleCount when not using 2x MSAA - its requirements for
  // OMSetRenderTargets need some investigation though).
  if (description.cull_mode == PipelineCullMode::kDisableRasterization) {
    state_desc.PS.pShaderBytecode = nullptr;
    state_desc.PS.BytecodeLength = 0;
    state_desc.DepthStencilState.DepthEnable = FALSE;
    state_desc.DepthStencilState.StencilEnable = FALSE;
  }

  // Create the D3D12 pipeline state object.
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  ID3D12PipelineState* state = nullptr;

  // Compute a unique LPCWSTR name for this PSO from the description hash so
  // ID3D12PipelineLibrary can key it. Same hash the .xpso file uses.
  const uint64_t pipeline_name_hash =
      XXH3_64bits(&description, sizeof(description));
  wchar_t pipeline_name[17];
  swprintf_s(pipeline_name, L"%016llX", static_cast<unsigned long long>(pipeline_name_hash));

  // Fast path: try LoadGraphicsPipeline from the persistent library. A hit
  // returns a fully compiled PSO in microseconds and skips the slow driver
  // compile entirely. ID3D12PipelineLibrary methods are not thread-safe per
  // spec, so all calls share pipeline_library_mutex_ - but Load is fast, so
  // contention is minimal even with multiple creation threads.
  if (pipeline_library_) {
    std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
    HRESULT hr_load = pipeline_library_->LoadGraphicsPipeline(
        pipeline_name, &state_desc, IID_PPV_ARGS(&state));
    if (SUCCEEDED(hr_load)) {
      pipeline_library_load_hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      state = nullptr;
    }
  }

  if (!state) {
    // Slow path: full driver compile. Stays OUTSIDE the library mutex so the
    // parallel creation thread pool retains its parallelism on cold boot.
    if (FAILED(device->CreateGraphicsPipelineState(&state_desc, IID_PPV_ARGS(&state)))) {
      if (runtime_description.pixel_shader != nullptr) {
        REXGPU_ERROR("Failed to create graphics pipeline with VS {:016X}, PS {:016X}",
                     runtime_description.vertex_shader->shader().ucode_data_hash(),
                     runtime_description.pixel_shader->shader().ucode_data_hash());
      } else {
        REXGPU_ERROR("Failed to create graphics pipeline with VS {:016X}",
                     runtime_description.vertex_shader->shader().ucode_data_hash());
      }
      return nullptr;
    }
    // Insert the freshly-compiled PSO into the library so the next boot can
    // skip the compile. Brief lock; just stamps a name into the in-memory
    // library tree. E_INVALIDARG (duplicate name from a parallel store) and
    // similar non-fatal returns are silently tallied.
    if (pipeline_library_) {
      std::lock_guard<std::mutex> lock(pipeline_library_mutex_);
      HRESULT hr_store = pipeline_library_->StorePipeline(pipeline_name, state);
      if (SUCCEEDED(hr_store)) {
        pipeline_library_store_count_.fetch_add(1, std::memory_order_relaxed);
        pipeline_library_dirty_.store(true, std::memory_order_relaxed);
        PsoCompileIndicatorStateRef().library_stores_session.fetch_add(
            1, std::memory_order_relaxed);
      } else {
        pipeline_library_store_failures_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  std::u16string name;
  if (runtime_description.pixel_shader != nullptr) {
    name = rex::string::to_utf16(fmt::format(
        "VS {:016X}, PS {:016X}", runtime_description.vertex_shader->shader().ucode_data_hash(),
        runtime_description.pixel_shader->shader().ucode_data_hash()));
  } else {
    name = rex::string::to_utf16(
        fmt::format("VS {:016X}", runtime_description.vertex_shader->shader().ucode_data_hash()));
  }
  state->SetName(reinterpret_cast<LPCWSTR>(name.c_str()));
  return state;
}

void PipelineCache::StorageWriteThread() {
  ShaderStoredHeader shader_header;
  // Don't leak anything in unused bits.
  std::memset(&shader_header, 0, sizeof(shader_header));

  std::vector<uint32_t> ucode_guest_endian;
  ucode_guest_endian.reserve(0xFFFF);

  bool flush_shaders = false;
  bool flush_pipelines = false;

  while (true) {
    if (flush_shaders) {
      flush_shaders = false;
      assert_not_null(shader_storage_file_);
      fflush(shader_storage_file_);
    }
    if (flush_pipelines) {
      flush_pipelines = false;
      assert_not_null(pipeline_storage_file_);
      fflush(pipeline_storage_file_);
    }

    const Shader* shader = nullptr;
    PipelineStoredDescription pipeline_description;
    bool write_pipeline = false;
    {
      std::unique_lock<std::mutex> lock(storage_write_request_lock_);
      if (storage_write_thread_shutdown_) {
        return;
      }
      if (!storage_write_shader_queue_.empty()) {
        shader = storage_write_shader_queue_.front();
        storage_write_shader_queue_.pop_front();
      } else if (storage_write_flush_shaders_) {
        storage_write_flush_shaders_ = false;
        flush_shaders = true;
      }
      if (!storage_write_pipeline_queue_.empty()) {
        std::memcpy(&pipeline_description, &storage_write_pipeline_queue_.front(),
                    sizeof(pipeline_description));
        storage_write_pipeline_queue_.pop_front();
        write_pipeline = true;
      } else if (storage_write_flush_pipelines_) {
        storage_write_flush_pipelines_ = false;
        flush_pipelines = true;
      }
      if (!shader && !write_pipeline) {
        storage_write_request_cond_.wait(lock);
        continue;
      }
    }

    if (shader) {
      shader_header.ucode_data_hash = shader->ucode_data_hash();
      shader_header.ucode_dword_count = shader->ucode_dword_count();
      shader_header.type = shader->type();
      assert_not_null(shader_storage_file_);
      fwrite(&shader_header, sizeof(shader_header), 1, shader_storage_file_);
      if (shader_header.ucode_dword_count) {
        ucode_guest_endian.resize(shader_header.ucode_dword_count);
        // Need to swap because the hash is calculated for the shader with guest
        // endianness.
        memory::copy_and_swap(ucode_guest_endian.data(), shader->ucode_dwords(),
                              shader_header.ucode_dword_count);
        fwrite(ucode_guest_endian.data(), shader_header.ucode_dword_count * sizeof(uint32_t), 1,
               shader_storage_file_);
      }
    }

    if (write_pipeline) {
      assert_not_null(pipeline_storage_file_);
      fwrite(&pipeline_description, sizeof(pipeline_description), 1, pipeline_storage_file_);
    }
  }
}

bool PipelineCache::PrepareRuntimeDescriptionForQueuedCreation(
    Pipeline* pipeline, PipelineRuntimeDescription& runtime_description) {
  assert_not_null(pipeline);
  std::memcpy(&runtime_description, &pipeline->description, sizeof(runtime_description));
  runtime_description.root_signature = pipeline->root_signature.load(std::memory_order_acquire);

  auto translate_pending_shader = [this](D3D12Shader::D3D12Translation* translation,
                                         const char* shader_type) -> bool {
    if (!translation) {
      return true;
    }
    if (!translation->is_translated()) {
      std::lock_guard<std::mutex> lock(translation_request_lock_);
      if (!translation->is_translated()) {
        translation->shader().AnalyzeUcode(ucode_disasm_buffer_);
        if (!TranslateAnalyzedShader(*shader_translator_, *translation, dxbc_converter_, dxc_utils_,
                                     dxc_compiler_)) {
          REXGPU_ERROR("Failed to translate queued {} shader", shader_type);
          return false;
        }
        if (shader_storage_file_ &&
            translation->shader().ucode_storage_index() != shader_storage_index_) {
          translation->shader().set_ucode_storage_index(shader_storage_index_);
          assert_not_null(storage_write_thread_);
          shader_storage_file_flush_needed_ = true;
          {
            std::lock_guard<std::mutex> storage_lock(storage_write_request_lock_);
            storage_write_shader_queue_.push_back(&translation->shader());
          }
          storage_write_request_cond_.notify_all();
        }
      }
    }
    return translation->is_valid();
  };

  if (pipeline->pending_vertex_shader) {
    D3D12Shader::D3D12Translation* pending_vertex = pipeline->pending_vertex_shader;
    pipeline->pending_vertex_shader = nullptr;
    if (!translate_pending_shader(pending_vertex, "vertex")) {
      return false;
    }
  }

  if (pipeline->pending_pixel_shader) {
    D3D12Shader::D3D12Translation* pending_pixel = pipeline->pending_pixel_shader;
    pipeline->pending_pixel_shader = nullptr;
    if (!translate_pending_shader(pending_pixel, "pixel")) {
      return false;
    }
    bool tessellated = Shader::IsHostVertexShaderTypeDomain(
        DxbcShaderTranslator::Modification(runtime_description.vertex_shader->modification())
            .vertex.host_vertex_shader_type);
    ID3D12RootSignature* root_signature = command_processor_.GetRootSignature(
        static_cast<const DxbcShader*>(&runtime_description.vertex_shader->shader()),
        static_cast<const DxbcShader*>(&pending_pixel->shader()), tessellated);
    if (!root_signature) {
      return false;
    }
    runtime_description.root_signature = root_signature;
    pipeline->root_signature.store(root_signature, std::memory_order_release);
  }

  return true;
}

void PipelineCache::CreationThread(size_t thread_index) {
  while (true) {
    Pipeline* pipeline_to_create = nullptr;

    // Check if need to shut down or set the completion event and dequeue the
    // pipeline if there is any.
    {
      std::unique_lock<std::mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ || creation_queue_.empty()) {
        if (creation_completion_set_event_ && creation_threads_busy_ == 0) {
          // Last pipeline in the queue created - signal the event if requested.
          creation_completion_set_event_ = false;
          creation_completion_event_->Set();
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      // Take the pipeline from the queue and increment the busy thread count
      // until the pipeline is created - other threads must be able to dequeue
      // requests, but can't set the completion event until the pipelines are
      // fully created (rather than just started creating).
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
      // Mirror to the cross-module indicator state. queue_size lags by one
      // until the lock is released, but the indicator polls at ImGui frame
      // cadence, so that's well below visible resolution.
      PsoCompileIndicatorStateRef().creation_queue_size.store(
          uint32_t(creation_queue_.size()), std::memory_order_relaxed);
      PsoCompileIndicatorStateRef().creation_threads_busy.store(
          uint32_t(creation_threads_busy_), std::memory_order_relaxed);
    }

    PipelineRuntimeDescription runtime_description;
    if (!PrepareRuntimeDescriptionForQueuedCreation(pipeline_to_create, runtime_description)) {
      pipeline_to_create->state.store(nullptr, std::memory_order_release);
    } else {
      auto compile_start = std::chrono::steady_clock::now();
      ID3D12PipelineState* pipeline_state = CreateD3D12Pipeline(runtime_description);
      uint64_t compile_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - compile_start)
              .count();
      pipeline_to_create->state.store(pipeline_state, std::memory_order_release);
      NoteAsyncCompile(compile_ns, runtime_description);
    }
    {
      // Progress bump is gated by the storage-init flag so runtime pipeline
      // creation (post-launch) doesn't bump the launch overlay's counters.
      auto& progress = command_processor_.graphics_system()->shader_storage_progress();
      if (progress.in_progress.load(std::memory_order_relaxed)) {
        progress.pipelines_created.fetch_add(1, std::memory_order_relaxed);
      }
    }

    // Indicator: mark this completion. Drives the "auto-hide N seconds after
    // last completion" timer in the overlay so a single mid-frame compile
    // gets a visible flash instead of disappearing instantly.
    {
      auto& ind = PsoCompileIndicatorStateRef();
      ind.total_completions_session.fetch_add(1, std::memory_order_relaxed);
      ind.last_completion_steady_ns.store(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count()),
          std::memory_order_relaxed);
    }

    // Pipeline created - the thread is not busy anymore, safe to set the
    // completion event if needed (at the next iteration, or in some other
    // thread).
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
      PsoCompileIndicatorStateRef().creation_threads_busy.store(
          uint32_t(creation_threads_busy_), std::memory_order_relaxed);
    }
  }
}

void PipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  auto& progress = command_processor_.graphics_system()->shader_storage_progress();
  const bool tracking = progress.in_progress.load(std::memory_order_relaxed);
  while (true) {
    Pipeline* pipeline_to_create;
    {
      std::lock_guard<std::mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
    }
    PipelineRuntimeDescription runtime_description;
    if (!PrepareRuntimeDescriptionForQueuedCreation(pipeline_to_create, runtime_description)) {
      pipeline_to_create->state.store(nullptr, std::memory_order_release);
      continue;
    }
    auto compile_start = std::chrono::steady_clock::now();
    ID3D12PipelineState* pipeline_state = CreateD3D12Pipeline(runtime_description);
    uint64_t compile_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - compile_start)
            .count();
    pipeline_to_create->state.store(pipeline_state, std::memory_order_release);
    NoteAsyncCompile(compile_ns, runtime_description);
    if (tracking) {
      progress.pipelines_created.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void PipelineCache::NoteAsyncCompile(uint64_t compile_ns,
                                     const PipelineRuntimeDescription& runtime_description) {
  stall_metrics_.frame_async_compile_ns.fetch_add(compile_ns, std::memory_order_relaxed);
  stall_metrics_.frame_async_completions.fetch_add(1, std::memory_order_relaxed);
  // Also update last_blocking_description_ so the per-frame log can show what
  // recently finished compiling, useful when the spike is caused by the
  // command processor skipping draws while waiting on this PSO.
  std::lock_guard<std::mutex> lock(last_blocking_description_mutex_);
  last_blocking_description_ = runtime_description.description;
  last_blocking_description_valid_ = true;
}

void PipelineCache::NoteDrawSkippedAwaitingAsyncPipeline() {
  stall_metrics_.frame_draws_skipped.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteRenderThreadBlockedOnAsyncPipeline(uint64_t wait_ns) {
  stall_metrics_.frame_block_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
  stall_metrics_.frame_blocks.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteSubmitNs(uint64_t submit_ns) {
  stall_metrics_.frame_submit_ns.fetch_add(submit_ns, std::memory_order_relaxed);
}

void PipelineCache::NotePresentNs(uint64_t present_ns) {
  stall_metrics_.frame_present_ns.fetch_add(present_ns, std::memory_order_relaxed);
}

void PipelineCache::NoteTextureRequestNs(uint64_t ns) {
  stall_metrics_.frame_texture_request_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_texture_request_calls.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteRtResolveNs(uint64_t ns) {
  stall_metrics_.frame_rt_resolve_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_rt_resolve_count.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteSubmitBarriersNs(uint64_t ns) {
  stall_metrics_.frame_submit_barriers_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_submit_barriers_calls.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteFenceWaitNs(uint64_t ns) {
  stall_metrics_.frame_fence_wait_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_fence_waits.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteFenceWaitQueueOpsNs(uint64_t ns) {
  stall_metrics_.frame_fence_wait_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_fence_waits.fetch_add(1, std::memory_order_relaxed);
  stall_metrics_.frame_fence_wait_qops_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_fence_wait_qops_count.fetch_add(1, std::memory_order_relaxed);
}

void PipelineCache::NoteFenceWaitSubmissionNs(uint64_t ns) {
  stall_metrics_.frame_fence_wait_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_fence_waits.fetch_add(1, std::memory_order_relaxed);
  stall_metrics_.frame_fence_wait_submission_ns.fetch_add(ns, std::memory_order_relaxed);
  stall_metrics_.frame_fence_wait_submission_count.fetch_add(1, std::memory_order_relaxed);
}

std::atomic<uint64_t>& PsoStallPresentNsAccumulator() {
  // Function-static to avoid the static-initialization-order fiasco. Defined
  // here (alongside PipelineCache, which is the sole reader) rather than in
  // d3d12_presenter.cpp so the writer side links cleanly without circular
  // dependency on the rexgraphics target.
  static std::atomic<uint64_t> accumulator{0};
  return accumulator;
}

PsoCompileIndicatorState& PsoCompileIndicatorStateRef() {
  // Function-static for the same reason as PsoStallPresentNsAccumulator
  // above. Read by PsoCompileIndicatorDialog in rexui; written from
  // PipelineCache's queue/busy/completion code paths.
  static PsoCompileIndicatorState state;
  return state;
}

}  // namespace rex::graphics::d3d12

namespace rex::perf {

GuestCpuStallMetrics& GuestCpuStallMetricsRef() {
  // Same function-static cross-module pattern. Writers: kernel TUs that
  // wrap NtCreate/Read/Write/Close, RtlAllocateHeap and friends, and the
  // NtWaitForSingleObject path. Reader/drainer: PipelineCache::ReportFrameBoundary.
  static GuestCpuStallMetrics metrics;
  return metrics;
}

}  // namespace rex::perf
