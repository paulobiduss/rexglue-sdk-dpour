#include <rex/perf/guest_cpu_stall_metrics.h>

namespace rex::perf {

GuestCpuStallMetrics& GuestCpuStallMetricsRef() {
  // Kernel instrumentation is backend-agnostic. Keeping this storage in core
  // makes it available to both the D3D12 and Vulkan runtime builds.
  static GuestCpuStallMetrics metrics;
  return metrics;
}

}  // namespace rex::perf
