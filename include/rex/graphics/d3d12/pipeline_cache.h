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

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

class PipelineCache {
 public:
  static constexpr size_t kLayoutUIDEmpty = 0;

  PipelineCache(D3D12CommandProcessor& command_processor, const RegisterFile& register_file,
                const D3D12RenderTargetCache& render_target_cache, bool bindless_resources_used);
  ~PipelineCache();

  bool Initialize();
  void Shutdown();
  // No ClearCache because it's undesirable with the persistent shader storage
  // (if the storage is reloaded, effectively nothing is cleared, while the call
  // takes a long time, and if it's not, there will be heavy stuttering for the
  // rest of the execution of the guest).

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking);
  void ShutdownShaderStorage();

  void EndSubmission();
  bool IsCreatingPipelines();

  // Per-frame metrics for correlating frame-time spikes with PSO/shader
  // compilation work on any thread. Counters are reset by ReportFrameBoundary()
  // each closed frame.
  struct PipelineStallMetrics {
    // Cache misses observed by ConfigurePipeline (sync + async).
    std::atomic<uint32_t> frame_misses{0};
    // Wall-clock CreateD3D12Pipeline time on the command-processor thread
    // (use_async == false branch). This is the ONLY value that directly
    // proves a render-thread stall.
    std::atomic<uint64_t> frame_sync_compile_ns{0};
    // Wall-clock CreateD3D12Pipeline time on background CreationThread(s) and
    // the on-processor drainer. Doesn't directly block render thread, but
    // burns CPU cycles other game threads (audio, logic) compete for, and is
    // the suspect when async_shader_compilation == true (Windows default).
    std::atomic<uint64_t> frame_async_compile_ns{0};
    std::atomic<uint32_t> frame_async_completions{0};
    // Draws that returned early in D3D12CommandProcessor::IssueDraw because
    // the async pipeline wasn't ready yet (policy=skip).
    std::atomic<uint32_t> frame_draws_skipped{0};
    // Cumulative wall-clock the render thread spent spin-waiting on a
    // pipeline state (policy=block).
    std::atomic<uint64_t> frame_block_wait_ns{0};
    std::atomic<uint32_t> frame_blocks{0};
    // ExecuteCommandLists and Present timing for the closed frame — set by
    // command_processor / presenter, drained in ReportFrameBoundary.
    std::atomic<uint64_t> frame_submit_ns{0};
    std::atomic<uint64_t> frame_present_ns{0};
    std::atomic<uint64_t> frame_vs_translate_ns{0};
    std::atomic<uint64_t> frame_ps_translate_ns{0};
    std::atomic<uint64_t> last_blocking_pso_hash{0};
    std::atomic<uint64_t> last_blocking_vs_hash{0};
    std::atomic<uint64_t> last_blocking_ps_hash{0};
    std::atomic<uint32_t> total_misses_session{0};
    // Non-PSO spike attribution (Etap 2). When a frame_ms spike fires with
    // pso_misses=0, these counters identify the bottleneck phase. All ns,
    // drained per frame in ReportFrameBoundary.
    std::atomic<uint64_t> frame_texture_request_ns{0};
    std::atomic<uint32_t> frame_texture_request_calls{0};
    std::atomic<uint64_t> frame_rt_resolve_ns{0};
    std::atomic<uint32_t> frame_rt_resolve_count{0};
    std::atomic<uint64_t> frame_submit_barriers_ns{0};
    std::atomic<uint32_t> frame_submit_barriers_calls{0};
    // Render-thread blocked waiting on submission_fence_->SetEventOnCompletion
    // → WaitForSingleObject(INFINITE). Dominant component of "unaccounted_ms"
    // when frames spike with pso_misses=0 — means GPU is busy and the CPU is
    // waiting for it. High values point to GPU-side bottleneck (texture
    // upload, heavy shader, driver work).
    std::atomic<uint64_t> frame_fence_wait_ns{0};
    std::atomic<uint32_t> frame_fence_waits{0};
    // Per-call-site split of frame_fence_wait_ns to localize which path
    // generates fence storms during spikes. Queue-ops = explicit
    // out-of-submission queue Signal+SetEventOnCompletion (mostly tile-mapping
    // updates). Submission = end-of-frame submission_fence_ wait (this is
    // where memexport readback Case A and end-of-frame CheckSubmissionFence
    // both funnel through). The two sub-counters sum to the totals above.
    std::atomic<uint64_t> frame_fence_wait_qops_ns{0};
    std::atomic<uint32_t> frame_fence_wait_qops_count{0};
    std::atomic<uint64_t> frame_fence_wait_submission_ns{0};
    std::atomic<uint32_t> frame_fence_wait_submission_count{0};
  };
  const PipelineStallMetrics& stall_metrics() const { return stall_metrics_; }
  // Called from D3D12CommandProcessor on each frame closure (is_swap &&
  // frame_open_). frame_duration_ns is the wall-clock delta between the
  // previous closure and this one.
  void ReportFrameBoundary(uint64_t frame_duration_ns);
  // Called by D3D12CommandProcessor::IssueDraw when a draw is dropped because
  // the PSO is still being compiled on a background creation thread (policy=skip).
  void NoteDrawSkippedAwaitingAsyncPipeline();
  // Called from IssueDraw after a spin-wait on pipeline.state (policy=block);
  // wait_ns is the wall-clock time the render thread spent waiting.
  void NoteRenderThreadBlockedOnAsyncPipeline(uint64_t wait_ns);
  // Called from D3D12CommandProcessor::EndSubmission around the
  // ExecuteCommandLists submit (one or more times per frame).
  void NoteSubmitNs(uint64_t submit_ns);
  // Called from the D3D12 presenter around IDXGISwapChain::Present.
  // Note: the presenter writes via the free-function
  // PsoStallPresentNsAccumulator() in pso_stall_present_accumulator.h to
  // avoid pulling the full PipelineCache header (and its xxhash transitive
  // dep) into the ui CMake target.
  void NotePresentNs(uint64_t present_ns);
  // Non-PSO spike attribution probes (Etap 2). Cheap fetch_add wrappers
  // around per-frame counters; drained in ReportFrameBoundary. Call from
  // each call-site wrapped with std::chrono::steady_clock pair around the
  // exact work being measured.
  void NoteTextureRequestNs(uint64_t ns);
  void NoteRtResolveNs(uint64_t ns);
  void NoteSubmitBarriersNs(uint64_t ns);
  void NoteFenceWaitNs(uint64_t ns);
  // Per-call-site variants. Each updates BOTH the matching sub-counter AND
  // the legacy frame_fence_wait_ns total so downstream consumers continue
  // working.
  void NoteFenceWaitQueueOpsNs(uint64_t ns);
  void NoteFenceWaitSubmissionNs(uint64_t ns);

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count);
  // Analyze shader microcode on the translator thread.
  void AnalyzeShaderUcode(Shader& shader) { shader.AnalyzeUcode(ucode_disasm_buffer_); }

  // Retrieves the shader modification for the current state. The shader must
  // have microcode analyzed.
  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this!
  bool ConfigurePipeline(D3D12Shader::D3D12Translation* vertex_shader,
                         D3D12Shader::D3D12Translation* pixel_shader,
                         const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                         reg::RB_DEPTHCONTROL normalized_depth_control,
                         uint32_t normalized_color_mask,
                         uint32_t bound_depth_and_color_render_target_bits,
                         const uint32_t* bound_depth_and_color_render_targets_formats,
                         void** pipeline_handle_out, ID3D12RootSignature** root_signature_out);

  // Returns a pipeline with deferred creation by its handle. May return nullptr
  // if failed to create the pipeline.
  ID3D12PipelineState* GetD3D12PipelineByHandle(void* handle) const {
    return reinterpret_cast<const Pipeline*>(handle)->state.load(std::memory_order_acquire);
  }

 private:
  REXPACKEDSTRUCT(ShaderStoredHeader, {
    uint64_t ucode_data_hash;

    uint32_t ucode_dword_count : 31;
    xenos::ShaderType type : 1;

    static constexpr uint32_t kVersion = 0x20201219;
  });

  // Update PipelineDescription::kVersion if any of the Pipeline* enums are
  // changed!

  enum class PipelineStripCutIndex : uint32_t {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };

  enum class PipelineTessellationMode : uint32_t {
    kNone,
    kDiscrete,
    kContinuous,
    kAdaptive,
  };

  enum class PipelinePatchType : uint32_t {
    kNone,
    kLine,
    kTriangle,
    kQuad,
  };

  enum class PipelinePrimitiveTopologyType : uint32_t {
    kPoint,
    kLine,
    kTriangle,
  };

  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };

  enum class PipelineCullMode : uint32_t {
    kNone,
    kFront,
    kBack,
    // Special case, handled via disabling the pixel shader and depth / stencil.
    kDisableRasterization,
  };

  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
  };

  // Update PipelineDescription::kVersion if anything is changed!
  REXPACKEDSTRUCT(PipelineRenderTarget, {
    uint32_t used : 1;                          // 1
    xenos::ColorRenderTargetFormat format : 4;  // 5
    PipelineBlendFactor src_blend : 4;          // 9
    PipelineBlendFactor dest_blend : 4;         // 13
    xenos::BlendOp blend_op : 3;                // 16
    PipelineBlendFactor src_blend_alpha : 4;    // 20
    PipelineBlendFactor dest_blend_alpha : 4;   // 24
    xenos::BlendOp blend_op_alpha : 3;          // 27
    uint32_t write_mask : 4;                    // 31
  });

  REXPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 if drawing without a pixel shader.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    int32_t depth_bias;
    float depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;  // 2
    // PipelinePrimitiveTopologyType for a vertex shader.
    // xenos::TessellationMode for a domain shader.
    uint32_t primitive_topology_type_or_tessellation_mode : 2;  // 4
    // Zero for non-kVertex host_vertex_shader_type.
    PipelineGeometryShader geometry_shader : 2;       // 6
    uint32_t fill_mode_wireframe : 1;                 // 7
    PipelineCullMode cull_mode : 2;                   // 9
    uint32_t front_counter_clockwise : 1;             // 10
    uint32_t depth_clip : 1;                          // 11
    xenos::MsaaSamples host_msaa_samples : 2;         // 13
    xenos::DepthRenderTargetFormat depth_format : 1;  // 14
    xenos::CompareFunction depth_func : 3;            // 17
    uint32_t depth_write : 1;                         // 18
    uint32_t stencil_enable : 1;                      // 19
    uint32_t stencil_read_mask : 8;                   // 27

    uint32_t stencil_write_mask : 8;                   // 8
    xenos::StencilOp stencil_front_fail_op : 3;        // 11
    xenos::StencilOp stencil_front_depth_fail_op : 3;  // 14
    xenos::StencilOp stencil_front_pass_op : 3;        // 17
    xenos::CompareFunction stencil_front_func : 3;     // 20
    xenos::StencilOp stencil_back_fail_op : 3;         // 23
    xenos::StencilOp stencil_back_depth_fail_op : 3;   // 26
    xenos::StencilOp stencil_back_pass_op : 3;         // 29
    xenos::CompareFunction stencil_back_func : 3;      // 32

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];

    static constexpr uint32_t kVersion = 0x20210425;
  });

  REXPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  struct PipelineRuntimeDescription {
    ID3D12RootSignature* root_signature;
    D3D12Shader::D3D12Translation* vertex_shader;
    D3D12Shader::D3D12Translation* pixel_shader;
    const std::vector<uint32_t>* geometry_shader;
    PipelineDescription description;
  };

  struct Pipeline;

  union GeometryShaderKey {
    uint32_t key;
    struct {
      PipelineGeometryShader type : 2;
      uint32_t interpolator_count : 5;
      uint32_t user_clip_plane_count : 3;
      uint32_t user_clip_plane_cull : 1;
      uint32_t has_vertex_kill_and : 1;
      uint32_t has_point_size : 1;
      uint32_t has_point_coordinates : 1;
      // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
      uint32_t point_ps_ucp_mode : 2;
    };

    GeometryShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

    struct Hasher {
      size_t operator()(const GeometryShaderKey& key) const {
        return std::hash<uint32_t>{}(key.key);
      }
    };
    bool operator==(const GeometryShaderKey& other_key) const { return key == other_key.key; }
    bool operator!=(const GeometryShaderKey& other_key) const { return !(*this == other_key); }
  };

  D3D12Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count, uint64_t data_hash);

  // Can be called from multiple threads.
  bool TranslateAnalyzedShader(DxbcShaderTranslator& translator,
                               D3D12Shader::D3D12Translation& translation,
                               IDxbcConverter* dxbc_converter = nullptr,
                               IDxcUtils* dxc_utils = nullptr,
                               IDxcCompiler* dxc_compiler = nullptr);

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this! The shaders must be translated
  // and valid unless for_placeholder is true.
  bool GetCurrentStateDescription(
      D3D12Shader::D3D12Translation* vertex_shader, D3D12Shader::D3D12Translation* pixel_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
      uint32_t bound_depth_and_color_render_target_bits,
      const uint32_t* bound_depth_and_color_render_target_formats,
      PipelineRuntimeDescription& runtime_description_out, bool for_placeholder = false);

  static bool GetGeometryShaderKey(PipelineGeometryShader geometry_shader_type,
                                   DxbcShaderTranslator::Modification vertex_shader_modification,
                                   DxbcShaderTranslator::Modification pixel_shader_modification,
                                   GeometryShaderKey& key_out);
  static void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);
  const std::vector<uint32_t>& GetGeometryShader(GeometryShaderKey key);

  ID3D12PipelineState* CreateD3D12Pipeline(const PipelineRuntimeDescription& runtime_description);
  bool PrepareRuntimeDescriptionForQueuedCreation(Pipeline* pipeline,
                                                  PipelineRuntimeDescription& runtime_description);

  // Background-path probes (CreationThread + drainer). Counterparts live in
  // PipelineStallMetrics; ReportFrameBoundary() drains them per frame.
  void NoteAsyncCompile(uint64_t compile_ns,
                        const PipelineRuntimeDescription& runtime_description);

  D3D12CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  const D3D12RenderTargetCache& render_target_cache_;
  bool bindless_resources_used_;

  // Temporary storage for AnalyzeUcode calls on the processor thread.
  string::StringBuffer ucode_disasm_buffer_;
  // Reusable shader translator for the processor thread.
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  std::mutex translation_request_lock_;

  // Command processor thread DXIL conversion/disassembly interfaces, if DXIL
  // disassembly is enabled.
  IDxbcConverter* dxbc_converter_ = nullptr;
  IDxcUtils* dxc_utils_ = nullptr;
  IDxcCompiler* dxc_compiler_ = nullptr;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, D3D12Shader*, rex::IdentityHasher<uint64_t>> shaders_;

  struct LayoutUID {
    size_t uid;
    size_t vector_span_offset;
    size_t vector_span_length;
  };
  std::mutex layouts_mutex_;
  // Texture binding layouts of different shaders, for obtaining layout UIDs.
  std::vector<D3D12Shader::TextureBinding> texture_binding_layouts_;
  // Map of texture binding layouts used by shaders, for obtaining UIDs. Keys
  // are XXH3 hashes of layouts, values need manual collision resolution using
  // layout_vector_offset:layout_length of texture_binding_layouts_.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      texture_binding_layout_map_;
  // Bindless sampler indices of different shaders, for obtaining layout UIDs.
  // For bindful, sampler count is used as the UID instead.
  std::vector<uint32_t> bindless_sampler_layouts_;
  // Keys are XXH3 hashes of used bindless sampler indices.
  std::unordered_multimap<uint64_t, LayoutUID, rex::IdentityHasher<uint64_t>>
      bindless_sampler_layout_map_;

  // Geometry shaders for Xenos primitive types not supported by Direct3D 12.
  std::unordered_map<GeometryShaderKey, std::vector<uint32_t>, GeometryShaderKey::Hasher>
      geometry_shaders_;

  // Empty depth-only pixel shader for writing to depth buffer via ROV when no
  // Xenos pixel shader provided.
  std::vector<uint8_t> depth_only_pixel_shader_;

  // Provenance of a Pipeline entry. Used for Phase-1 capture summary so
  // sessions can answer "how many of the PSOs I encountered today were
  // prewarmed from the shareable .xpso, vs cold-compiled mid-gameplay".
  // Stored only in-memory; not written back into the existing .xpso binary
  // format (that format stays byte-stable for older builds).
  enum class PipelineOrigin : uint8_t {
    kUnknown = 0,
    kBootReplay = 1,    // created during InitializeShaderStorage cache replay
    kRuntimeMiss = 2,   // first encounter happened during gameplay
  };

  // Live usage stats for a Pipeline. Atomic because creation threads, the
  // command-processor thread, and the cache hit path can all touch a
  // pipeline at overlapping times. Reset per session (not persisted).
  struct PipelineMetadata {
    std::atomic<uint64_t> usage_count{0};
    std::atomic<uint64_t> first_frame{UINT64_MAX};
    std::atomic<uint64_t> last_frame{0};
    PipelineOrigin origin = PipelineOrigin::kUnknown;
    // True iff the Pipeline was first observed via a runtime miss in
    // ConfigurePipeline (i.e. NOT pre-populated from cache replay). Drives
    // the "Runtime misses" counter in the end-of-session summary.
    bool caused_runtime_miss = false;
  };

  struct Pipeline {
    // nullptr if creation has failed.
    std::atomic<ID3D12PipelineState*> state{nullptr};
    std::atomic<ID3D12RootSignature*> root_signature{nullptr};
    PipelineRuntimeDescription description;
    D3D12Shader::D3D12Translation* pending_vertex_shader = nullptr;
    D3D12Shader::D3D12Translation* pending_pixel_shader = nullptr;
    uint8_t priority = 0;
    PipelineMetadata metadata;
  };
  struct PipelineCreationPriorityComparator {
    bool operator()(const Pipeline* a, const Pipeline* b) const {
      uint8_t priority_a = a ? a->priority : 0;
      uint8_t priority_b = b ? b->priority : 0;
      return priority_a < priority_b;
    }
  };
  // All previously generated pipelines identified by hash and the description.
  std::unordered_multimap<uint64_t, Pipeline*, rex::IdentityHasher<uint64_t>> pipelines_;

  // Previously used pipeline. This matches our current state settings and
  // allows us to quickly(ish) reuse the pipeline if no registers have been
  // changed.
  Pipeline* current_pipeline_ = nullptr;

  // Currently open shader storage path.
  std::filesystem::path shader_storage_cache_root_;
  uint32_t shader_storage_title_id_ = 0;

  // Shader storage output stream, for preload in the next emulator runs.
  FILE* shader_storage_file_ = nullptr;
  // For only writing shaders to the currently open storage once, incremented
  // when switching the storage.
  uint32_t shader_storage_index_ = 0;
  bool shader_storage_file_flush_needed_ = false;

  // Pipeline storage output stream, for preload in the next emulator runs.
  FILE* pipeline_storage_file_ = nullptr;
  bool pipeline_storage_file_flush_needed_ = false;

  // Thread for asynchronous writing to the storage streams.
  void StorageWriteThread();
  std::mutex storage_write_request_lock_;
  std::condition_variable storage_write_request_cond_;
  // Storage thread input is protected with storage_write_request_lock_, and the
  // thread is notified about its change via storage_write_request_cond_.
  std::deque<const Shader*> storage_write_shader_queue_;
  std::deque<PipelineStoredDescription> storage_write_pipeline_queue_;
  bool storage_write_flush_shaders_ = false;
  bool storage_write_flush_pipelines_ = false;
  bool storage_write_thread_shutdown_ = false;
  std::unique_ptr<rex::thread::Thread> storage_write_thread_;

  // Pipeline creation threads.
  void CreationThread(size_t thread_index);
  void CreateQueuedPipelinesOnProcessorThread();
  std::mutex creation_request_lock_;
  std::condition_variable creation_request_cond_;
  // Protected with creation_request_lock_, notify_one creation_request_cond_
  // when set.
  std::priority_queue<Pipeline*, std::vector<Pipeline*>, PipelineCreationPriorityComparator>
      creation_queue_;
  // Number of threads that are currently creating a pipeline - incremented when
  // a pipeline is dequeued (the completion event can't be triggered before this
  // is zero). Protected with creation_request_lock_.
  size_t creation_threads_busy_ = 0;
  // Manual-reset event set when the last queued pipeline is created and there
  // are no more pipelines to create. This is triggered by the thread creating
  // the last pipeline.
  std::unique_ptr<rex::thread::Event> creation_completion_event_;
  // Whether setting the event on completion is queued. Protected with
  // creation_request_lock_, notify_one creation_request_cond_ when set.
  bool creation_completion_set_event_ = false;
  // Creation threads with this index or above need to be shut down as soon as
  // possible. Protected with creation_request_lock_, notify_all
  // creation_request_cond_ when set.
  size_t creation_threads_shutdown_from_ = SIZE_MAX;
  std::vector<std::unique_ptr<rex::thread::Thread>> creation_threads_;

  // Stall instrumentation (see ReportFrameBoundary in the public section).
  PipelineStallMetrics stall_metrics_;
  std::atomic<uint32_t> spike_frame_counter_{0};
  // Full description of the most recent PSO that blocked the command-processor
  // thread on a synchronous compile. Stored under a mutex (rare update, rare
  // read) so the per-frame log can dump every field we care about for the
  // future cache/prewarm step (RT formats, depth/stencil state, blend hashes).
  std::mutex last_blocking_description_mutex_;
  PipelineDescription last_blocking_description_{};
  bool last_blocking_description_valid_ = false;

  // ID3D12PipelineLibrary blob cache. Caches driver-compiled PSO microcode
  // across runs on the same GPU/driver. LoadGraphicsPipeline hit on a hot
  // pipeline is ~5-50 us; cold compile is 100-800 ms for ROV PSOs in this
  // title. Spec requires single-threaded access to the library, so calls are
  // serialized under pipeline_library_mutex_; the expensive
  // CreateGraphicsPipelineState() fallback stays OUTSIDE the lock so the
  // existing parallel creation thread pool keeps its parallelism on cold
  // boot. The blob memory must outlive pipeline_library_, so the read buffer
  // is kept in pipeline_library_blob_.
  Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> pipeline_library_;
  std::vector<uint8_t> pipeline_library_blob_;
  std::filesystem::path pipeline_library_path_;
  std::mutex pipeline_library_mutex_;
  std::atomic<uint32_t> pipeline_library_load_hits_{0};
  std::atomic<uint32_t> pipeline_library_store_count_{0};
  std::atomic<uint32_t> pipeline_library_store_failures_{0};
  std::atomic<bool> pipeline_library_dirty_{false};

  // Phase-1 portable-cache metadata. Sidecar `*.xpso.meta.toml` carries the
  // header (versions, hashes); per-pipeline usage stats live in-memory only.
  std::filesystem::path pipeline_metadata_sidecar_path_;
  std::atomic<uint64_t> session_frame_counter_{0};
  // Session-summary counters - written into the end-of-session log line in
  // ShutdownShaderStorage. Set/incremented from creation + hit paths.
  std::atomic<uint64_t> session_portable_keys_loaded_{0};
  std::atomic<uint64_t> session_prewarmed_pipelines_{0};
  std::atomic<uint64_t> session_runtime_misses_{0};
  std::atomic<uint64_t> session_spikes_over_16ms_{0};
  std::atomic<uint64_t> session_spikes_over_100ms_{0};
  std::atomic<uint64_t> session_new_keys_captured_{0};
  bool sidecar_metadata_mismatch_ = false;
  // Serialize pipeline_library_ to disk. Safe to call from any thread; takes
  // pipeline_library_mutex_ internally. No-op if the library is not dirty.
  // Used after InitializeShaderStorage (so PSOs populated from cached
  // descriptions are captured even on hard kill) and at clean shutdown.
  void FlushPipelineLibraryToDisk();

  // Write portable-database sidecar `*.xpso.meta.toml`. Called from both the
  // end of InitializeShaderStorage (so hard kill mid-session still leaves a
  // valid sidecar describing the on-disk cache) and clean shutdown.
  void WriteSidecarMetaTomlToDisk();
};

}  // namespace rex::graphics::d3d12
