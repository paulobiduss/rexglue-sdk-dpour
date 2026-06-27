/**
 * Lightweight cross-module hooks for instrumenting the GUEST CPU thread.
 *
 * Class B "unaccounted_ms" frame spikes (200-358 ms residuals on otherwise
 * idle render paths) are not visible to PipelineCache's render-thread
 * counters. They reflect time the statically-recompiled PPC code spends
 * blocking inside the kernel emulation layer — file I/O retries,
 * RtlAllocateHeap fragments, NtWaitForSingleObject on guest events, etc.
 *
 * This header exposes three atomic counter pairs (ns + count) drained per
 * frame by PipelineCache::ReportFrameBoundary. The kernel TUs increment them
 * via a tiny RAII scoped timer; cost ≈ 30 ns RDTSC pair per call.
 *
 * Storage lives next to PsoStallPresentNsAccumulator in pipeline_cache.cpp so
 * the kernel CMake target doesn't have to depend on the full PipelineCache
 * header (which transitively pulls xxhash + lots of graphics types).
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::perf {

struct GuestCpuStallMetrics {
  // Aggregated time the guest spent inside NtCreateFile / NtReadFile /
  // NtWriteFile / NtClose etc. since the last drain.
  std::atomic<uint64_t> frame_kernel_io_ns{0};
  std::atomic<uint32_t> frame_kernel_io_calls{0};

  // Aggregated time in RtlAllocateHeap / RtlFreeHeap / RtlReAllocateHeap.
  std::atomic<uint64_t> frame_alloc_ns{0};
  std::atomic<uint32_t> frame_alloc_calls{0};

  // Aggregated time in NtWaitForSingleObjectEx / KeWaitForMultipleObjects.
  // This is GUEST-thread block time on guest objects (events, semaphores,
  // mutexes). High values point to deadlock-style contention.
  std::atomic<uint64_t> frame_wait_ns{0};
  std::atomic<uint32_t> frame_wait_count{0};
};

// Single process-wide instance. Storage in pipeline_cache.cpp.
GuestCpuStallMetrics& GuestCpuStallMetricsRef();

enum class GuestCpuBucket : uint8_t {
  kKernelIo,
  kAlloc,
  kWait,
};

// RAII scoped timer. Drop one of these at the top of a kernel-call entry
// function; on destruction it adds elapsed nanoseconds + 1 call to the
// bucket. Header-only so call sites don't go through a function pointer.
struct ScopedGuestCpuTimer {
  GuestCpuBucket bucket;
  std::chrono::steady_clock::time_point start;

  explicit ScopedGuestCpuTimer(GuestCpuBucket b)
      : bucket(b), start(std::chrono::steady_clock::now()) {}

  ~ScopedGuestCpuTimer() {
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
    auto& m = GuestCpuStallMetricsRef();
    switch (bucket) {
      case GuestCpuBucket::kKernelIo:
        m.frame_kernel_io_ns.fetch_add(static_cast<uint64_t>(elapsed_ns),
                                       std::memory_order_relaxed);
        m.frame_kernel_io_calls.fetch_add(1, std::memory_order_relaxed);
        break;
      case GuestCpuBucket::kAlloc:
        m.frame_alloc_ns.fetch_add(static_cast<uint64_t>(elapsed_ns),
                                    std::memory_order_relaxed);
        m.frame_alloc_calls.fetch_add(1, std::memory_order_relaxed);
        break;
      case GuestCpuBucket::kWait:
        m.frame_wait_ns.fetch_add(static_cast<uint64_t>(elapsed_ns),
                                   std::memory_order_relaxed);
        m.frame_wait_count.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }
};

}  // namespace rex::perf
