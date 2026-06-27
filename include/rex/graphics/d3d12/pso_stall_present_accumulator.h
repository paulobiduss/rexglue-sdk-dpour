/**
 * Lightweight cross-module hook for the PSO stall instrumentation Present
 * timing. d3d12_presenter.cpp lives in a CMake target (rexui) that does not
 * see pipeline_cache.h's transitive xxhash dependency, so it cannot pull the
 * full PipelineCache header just to publish a single timing accumulator.
 *
 * This header carries only the accessor declaration. The actual storage and
 * implementation live alongside PipelineCache in pipeline_cache.cpp.
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics::d3d12 {

// Cumulative wall-clock nanoseconds spent inside IDXGISwapChain::Present
// since the last drain by PipelineCache::ReportFrameBoundary().
std::atomic<uint64_t>& PsoStallPresentNsAccumulator();

}  // namespace rex::graphics::d3d12
