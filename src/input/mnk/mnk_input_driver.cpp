/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <rex/ui/window_win.h>
#include <Windows.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_capture_mouse, false, "Input",
                    "Capture and hide mouse cursor while MnK is active");
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK").range(0, 3);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 0.6, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
// How responsive the stick is to fresh mouse input. 0 = no smoothing (instant
// response, can feel twitchy on integer-pixel WM_MOUSEMOVE); 0.95 = heavy
// smoothing (silky but laggy). 0.15 is the new lower-lag default.
REXCVAR_DEFINE_DOUBLE(mnk_smoothing, 0.15, "Input",
                      "Mouse-to-stick EMA smoothing (0=no smoothing, 0.95=heavy)")
    .range(0.0, 0.95);
// Sub-linear curve: output ~ |delta|^exp * sign(delta). 1.0 = linear, 1.4 gives
// finer precision at small movements and stronger response at large flicks
// (acceleration-like feel without Windows ballistics interference).
REXCVAR_DEFINE_DOUBLE(mnk_acceleration_exponent, 1.0, "Input",
                      "Mouse acceleration curve exponent (1.0=linear, 1.5=precision boost)")
    .range(0.5, 2.5);
// How fast the stick decays back to 0 when the mouse stops. 0 = instant snap
// to centre, 0.9 = long coast. 0.30 default eliminates the lingering drift the
// older 0.5 default produced after a fast flick.
REXCVAR_DEFINE_DOUBLE(mnk_decay, 0.30, "Input",
                      "Stick decay rate when mouse stops (0=snap, 0.9=long coast)")
    .range(0.0, 0.9);
// Deadzone floor: smallest movement maps to this stick value, scaled up
// linearly from 0 over an initial smooth ramp window. Lower = more direct
// small-movement response; higher = quicker over-the-deadzone activation.
REXCVAR_DEFINE_INT32(mnk_deadzone_compensation, 4000, "Input",
                     "Stick deadzone compensation in int16 units (smooth ramp)")
    .range(0, 16000);
REXCVAR_DEFINE_BOOL(mnk_invert_y, false, "Input", "Invert mouse Y axis");

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "C", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "E", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "F", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "R", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "Shift", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller",
                      "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

using rex::ui::VirtualKey;

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  if (attached_window_) {
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

X_STATUS MnkInputDriver::Setup() {
  REXLOG_INFO("MnK input driver initialized");
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    attached_window_ = window;
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    if (mouse_captured_) {
      mouse_captured_ = false;
      attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      attached_window_->ReleaseMouse();
    }
    attached_window_->RemoveInputListener(this);
    attached_window_->RemoveListener(this);
    attached_window_ = nullptr;
  }
}

uint32_t MnkInputDriver::UserIndex() const {
  return static_cast<uint32_t>(REXCVAR_GET(mnk_user_index));
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

void MnkInputDriver::ClearStateLocked() {
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
}

// Trim ASCII whitespace from both ends of [first, last). Returns the new range.
static std::string_view TrimAsciiSpace(std::string_view s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return s.substr(a, b - a);
}

static bool IsSingleKeyDown(const bool (&key_down)[256], std::string_view key_name) {
  std::string trimmed_owned(TrimAsciiSpace(key_name));
  if (trimmed_owned.empty())
    return false;
  VirtualKey vk = rex::ui::ParseVirtualKey(trimmed_owned);
  if (vk == VirtualKey::kNone)
    return false;
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

// Evaluates a bind expression. Supports:
//   "E"               -> A is pressed iff E is down.
//   "E,LMB"           -> A is pressed iff E OR LMB is down.
//   "Space+LMB"       -> A is pressed iff Space AND LMB are both down.
//   "E,Space+LMB"     -> A is pressed iff E is down OR (Space AND LMB) are down.
// Whitespace inside tokens is trimmed. Empty / unparsable tokens silently
// disqualify their containing AND-group.
static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val) {
  std::string_view rest = cvar_val;
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view group =
        TrimAsciiSpace(rest.substr(0, comma == std::string_view::npos ? rest.size() : comma));
    rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);

    if (group.empty())
      continue;

    bool all_down = true;
    bool any_key = false;
    std::string_view inner = group;
    while (!inner.empty()) {
      size_t plus = inner.find('+');
      std::string_view key =
          TrimAsciiSpace(inner.substr(0, plus == std::string_view::npos ? inner.size() : plus));
      inner = (plus == std::string_view::npos) ? std::string_view{} : inner.substr(plus + 1);
      if (key.empty())
        continue;
      any_key = true;
      if (!IsSingleKeyDown(key_down, key)) {
        all_down = false;
        break;
      }
    }

    if (any_key && all_down)
      return true;
  }
  return false;
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  if (!IsEnabled()) {
    std::lock_guard lock(state_mutex_);
    ClearStateLocked();
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  UpdateMouseCapture();

  if (!is_active() || !has_focus_) {
    std::lock_guard lock(state_mutex_);
    ClearStateLocked();
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  std::lock_guard lock(state_mutex_);

  // Drain one mouse-wheel detent per poll and pulse the synthetic
  // kWheelUp / kWheelDown keys for exactly this frame so binds referencing
  // "WheelUp" / "WheelDown" register as one button press per scroll detent.
  constexpr int32_t kWheelDetent = 120;  // MouseEvent::kScrollPerDetent
  bool wheel_up_pulse = false;
  bool wheel_down_pulse = false;
  if (wheel_accumulator_y_ >= kWheelDetent) {
    wheel_accumulator_y_ -= kWheelDetent;
    wheel_up_pulse = true;
  } else if (wheel_accumulator_y_ <= -kWheelDetent) {
    wheel_accumulator_y_ += kWheelDetent;
    wheel_down_pulse = true;
  }
  key_down_[static_cast<uint8_t>(rex::ui::VirtualKey::kWheelUp)] = wheel_up_pulse;
  key_down_[static_cast<uint8_t>(rex::ui::VirtualKey::kWheelDown)] = wheel_down_pulse;

  uint16_t buttons = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_a)))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_b)))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_x)))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_y)))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_left_shoulder)))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_right_shoulder)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_press)))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_rstick_press)))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_back)))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_start)))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_guide)))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_up)))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_down)))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_left)))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_dpad_right)))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(key_down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(key_down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_left)))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_right)))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_up)))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, REXCVAR_GET(keybind_lstick_down)))
    ly -= INT16_MAX;

  double sensitivity = REXCVAR_GET(mnk_sensitivity);
  double smoothing = std::clamp(REXCVAR_GET(mnk_smoothing), 0.0, 0.95);
  double accel_exp = std::clamp(REXCVAR_GET(mnk_acceleration_exponent), 0.5, 2.5);
  double decay = std::clamp(REXCVAR_GET(mnk_decay), 0.0, 0.9);
  double deadzone_floor = std::clamp(
      static_cast<double>(REXCVAR_GET(mnk_deadzone_compensation)), 0.0, 16000.0);
  bool invert_y = REXCVAR_GET(mnk_invert_y);
  // Base scale tuned for sub-linear curve. Lower than the old 2500 because the
  // pow(|dx|, exp) curve amplifies large flicks anyway, so sensitivity 0.6
  // gives a comfortable PC-mouse feel that doesn't oversteer on small twitches.
  constexpr double kBaseScale = 1500.0;
  // Apply acceleration curve: sub-linear small movements (more precision),
  // super-linear large movements (snappier turns).
  auto curved = [&](int32_t dpx) {
    if (dpx == 0) return 0.0;
    double mag = std::pow(std::abs(static_cast<double>(dpx)), accel_exp);
    return std::copysign(mag, static_cast<double>(dpx));
  };
  double new_rx = curved(mouse_dx_) * sensitivity * kBaseScale;
  double new_ry = curved(mouse_dy_) * sensitivity * kBaseScale;
  // Stick Y is up=positive, screen Y is down=positive, so invert by default.
  // mnk_invert_y flips back to "down on mouse = look down on stick" feel.
  new_ry = invert_y ? new_ry : -new_ry;
  mouse_dx_ = 0;
  mouse_dy_ = 0;

  // Mouse velocity → stick mapping with a one-pole low-pass filter.
  // `smoothing` is the EMA carry-over alpha; lower = more responsive but more
  // raw twitchy variance. Default 0.15 keeps ~2-3 ms perceived lag.
  constexpr double kInt16Max = 32767.0;
  auto update_stick_axis = [&](double new_v, double& stick) {
    if (new_v != 0.0) {
      double target = std::clamp(new_v, -kInt16Max, kInt16Max);
      stick = stick * smoothing + target * (1.0 - smoothing);
    } else {
      stick *= decay;
      if (std::abs(stick) < 1.0) stick = 0.0;
    }
  };
  update_stick_axis(new_rx, mouse_stick_x_);
  update_stick_axis(new_ry, mouse_stick_y_);

  // Smooth-ramp deadzone compensation. The hard-jump from 0 to ~9500 in the
  // old code was a major source of the "stick-not-mouse" feel: 1 stray pixel
  // would catapult the stick a third of the way to max. Now we linearly ramp
  // 0..kRampWindow into 0..deadzone_floor, then linearly to kInt16Max above.
  constexpr double kRampWindow = 800.0;
  auto remap_axis = [&](double v) -> int32_t {
    if (v == 0.0) return 0;
    double mag = std::min(std::abs(v), kInt16Max);
    double scaled;
    if (mag < kRampWindow) {
      scaled = (mag / kRampWindow) * deadzone_floor;
    } else {
      double t = (mag - kRampWindow) / (kInt16Max - kRampWindow);
      scaled = deadzone_floor + t * (kInt16Max - deadzone_floor);
    }
    return static_cast<int32_t>(std::copysign(scaled, v));
  };
  int32_t rx = remap_axis(mouse_stick_x_);
  int32_t ry = remap_axis(mouse_stick_y_);

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled()) {
    std::lock_guard lock(state_mutex_);
    ClearStateLocked();
    while (!keystroke_queue_.empty()) {
      keystroke_queue_.pop();
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index != UserIndex()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (!is_active() || !has_focus_) {
    ClearStateLocked();
    while (!keystroke_queue_.empty()) {
      keystroke_queue_.pop();
    }
    return X_ERROR_EMPTY;
  }
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(UserIndex());
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::CenterCursor() {
  if (!attached_window_)
    return;
  int32_t cx = static_cast<int32_t>(attached_window_->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(attached_window_->GetActualLogicalHeight() / 2);
  prev_mouse_x_ = cx;
  prev_mouse_y_ = cy;
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(attached_window_);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture() {
  if (!attached_window_)
    return;

  bool should_capture = IsEnabled() && has_focus_ && is_active() && REXCVAR_GET(mnk_capture_mouse);

  if (should_capture && !mouse_captured_) {
    mouse_captured_ = true;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    attached_window_->CaptureMouse();
    // Reset deltas to avoid a spike on capture start
    mouse_dx_ = 0;
    mouse_dy_ = 0;
  } else if (!should_capture && mouse_captured_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }

  // Re-center cursor each frame while captured to prevent edge clamping
  if (mouse_captured_) {
    CenterCursor();
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_ || !is_active()) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || !is_active())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_ || !is_active())
    return;
  std::lock_guard lock(state_mutex_);
  int32_t x = e.x();
  int32_t y = e.y();
  mouse_dx_ += x - prev_mouse_x_;
  mouse_dy_ += y - prev_mouse_y_;
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  wheel_accumulator_y_ += e.scroll_y();
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = false;
  ClearStateLocked();
  if (mouse_captured_ && attached_window_) {
    mouse_captured_ = false;
    attached_window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    attached_window_->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk
