#include <catch2/catch_test_macros.hpp>

#include <rex/perf/guest_cpu_stall_metrics.h>

TEST_CASE("guest CPU timers update process-wide metrics", "[core][perf]") {
  auto& metrics = rex::perf::GuestCpuStallMetricsRef();
  metrics.frame_kernel_io_ns.store(0, std::memory_order_relaxed);
  metrics.frame_kernel_io_calls.store(0, std::memory_order_relaxed);

  {
    rex::perf::ScopedGuestCpuTimer timer(rex::perf::GuestCpuBucket::kKernelIo);
  }

  REQUIRE(metrics.frame_kernel_io_calls.load(std::memory_order_relaxed) == 1);
}
