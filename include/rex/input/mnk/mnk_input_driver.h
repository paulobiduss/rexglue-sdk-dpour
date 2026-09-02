/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  // Per-pad-button slot index for keystroke edge tracking and repeat timing.
  enum PadIdx {
    kPadIdxA = 0,
    kPadIdxB,
    kPadIdxX,
    kPadIdxY,
    kPadIdxLB,
    kPadIdxRB,
    kPadIdxStart,
    kPadIdxBack,
    kPadIdxL3,
    kPadIdxR3,
    kPadIdxDU,
    kPadIdxDD,
    kPadIdxDL,
    kPadIdxDR,
    kPadIdxLT,
    kPadIdxRT,
    kPadIdxLStick,
    kPadIdxRStick,
    kPadIdxCount
  };

 private:
  uint32_t UserIndex() const;
  bool IsEnabled() const;
  void CenterCursor();
  // DPOUR MIGRATION 2026-09-02 (upstream b458afe, adapted): GetState runs on
  // the guest thread, but every Window call in the capture path reaches the
  // platform window. The guest side only queues the desired state; the apply
  // runs on the UI thread, coalesced to one post in flight.
  void QueueMouseCaptureUpdate(bool should_capture);
  void ApplyMouseCaptureFromUIThread();
  void SetKeyState(uint16_t vk, bool down);
  // PR #311: flags = X_INPUT_KEYSTROKE_KEYDOWN / KEYUP / REPEAT, etc.
  // dpour: kept our old simpler signature replaced by the flags version
  // since our HEAD had no live callers — purely a header decl.
  void EnqueueKeystroke(uint16_t vk_pad, uint16_t flags);
  void HandleEdge(PadIdx idx, uint16_t vk_pad, bool down);
  void HandleStickDirChange(PadIdx idx, uint16_t new_dir);
  void EmitButtonChange(rex::ui::VirtualKey key_vk, bool down);
  void RecomputeLstickDir();
  void EnqueueRStickIfChanged(int16_t rx, int16_t ry);
  void TickRepeats();
  void ClearStateLocked();

  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};

  // Mouse delta tracking
  int32_t mouse_dx_ = 0;
  int32_t mouse_dy_ = 0;
  // DPOUR MIGRATION 2026-09-02: raw-input (WM_INPUT) delta accumulators.
  // Filled by the window's raw-motion callback (UI thread), drained by
  // GetState (guest thread) - both under state_mutex_.
  float raw_mouse_dx_ = 0.0f;
  float raw_mouse_dy_ = 0.0f;
  // True while the window delivers raw motion; UI thread writes.
  std::atomic<bool> raw_input_active_{false};
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  // Written on the UI thread; the guest thread reads it in the queue-skip test.
  std::atomic<bool> mouse_captured_{false};
  bool has_focus_ = true;

  // Guest thread to UI thread; the queued flag coalesces the posts.
  std::atomic<bool> mouse_capture_requested_{false};
  std::atomic<bool> mouse_capture_update_queued_{false};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Vertical mouse-wheel accumulator (Windows WHEEL_DELTA units, 120/detent).
  // Drained one detent per GetState poll, pulses kWheelUp/kWheelDown for one
  // frame each. Lets the wheel be bound to any controller button.
  int32_t wheel_accumulator_y_ = 0;

  // Right-stick virtual value. Refreshed each poll by raw mouse motion
  // (delta * sensitivity * scale), or decayed toward zero when the mouse is
  // stationary so a brief flick still has a clean tail.
  double mouse_stick_x_ = 0.0;
  double mouse_stick_y_ = 0.0;

  // PR #311: per-pad-button state for KEYDOWN/KEYUP edge tracking and
  // KEYSTROKE_REPEAT timing. Stick slots store the currently-held direction
  // as vk_pad. ClearStateLocked() must reset these alongside key_down_.
  struct PadKeyState {
    bool held = false;
    uint16_t vk_pad = 0;  // VirtualKey value, 0 = kNone
    std::chrono::steady_clock::time_point pressed_at;
    std::chrono::steady_clock::time_point last_event_at;
  };
  PadKeyState pad_states_[kPadIdxCount];

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
