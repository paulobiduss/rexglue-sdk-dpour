/**
 ******************************************************************************
 * ReXGlue : rex::graphics native guest-output renderer - INERT STUB          *
 ******************************************************************************
 * dpour-fork 2026-09-02: the native render layer is RETIRED by owner
 * decision (see downpour repo commit b480f9e). The app side no longer
 * registers a renderer, so every hook below was already dead at runtime:
 * g_native_output_active could never become true. This stub keeps the
 * command-processor call sites linking while removing the
 * native_render_suppress_* cvars from the settings UI and config space.
 * The full implementation is preserved in _archived_native_render_sdk/.
 */

#include <rex/graphics/native_guest_renderer.h>

namespace rex::graphics {

void SetNativeGuestOutputRenderer(NativeGuestOutputRenderer, void*) {}

bool TryRenderNativeGuestOutput(const NativeGuestOutputRenderContext&) {
  return false;
}

bool HasNativeGuestOutputRenderer() {
  return false;
}

bool IsNativeGuestOutputActive() {
  return false;
}

void SetNativeGuestOutputWideAspect(double) {}

double GetNativeGuestOutputWideAspect() {
  return 0.0;
}

bool ApplyNativeGuestOutputWideAspect(uint32_t&, uint32_t, uint32_t&, uint32_t&) {
  return false;
}

bool ShouldSuppressEmulatedDraws() {
  return false;
}

bool ShouldSuppressPassAtPitch(uint32_t) {
  return false;
}

bool ShouldSuppressExemptDepthOnlyDraws() {
  return false;
}

void SetNativeGuestOutputPostProcessor(NativeGuestOutputPostProcessor, void*) {}

bool HasNativeGuestOutputPostProcessor() {
  return false;
}

void InvokeNativeGuestOutputPostProcessor(const NativeGuestOutputRenderContext&) {}

void RequestNativeGuestOutputPostProcess(bool) {}

bool IsNativeGuestOutputPostProcessRequested() {
  return false;
}

}  // namespace rex::graphics

namespace rex::graphics::nrhi {

// Referenced by the (dead but still linked) native RHI implementation.
void SetShaderBytecodeCacheDirectory(const char*) {}

const char* GetShaderBytecodeCacheDirectory() {
  return "";
}

}  // namespace rex::graphics::nrhi
