/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <cstdlib>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/fps_overlay.h>
#include <rex/ui/overlay/pso_compile_indicator_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/overlay/shader_compile_overlay.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
// dpour-fork: gpu_plugin.h is a nightly-era header (plugin split for GPU
// backend). v1.1.6 SDK compiles D3D12 directly into rexruntime — no plugin
// dispatch needed. Removed to unblock v1.1.7 achievement port.
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#if REX_PLATFORM_LINUX
#include <gnu/libc-version.h>
#include <sys/utsname.h>
#endif

#include <fmt/format.h>
#include <imgui.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace rex {

namespace {

constexpr bool kBlockShaderStorageStartup =
#if REX_PLATFORM_WIN32 || REX_PLATFORM_MAC
    // macOS needs this as much as Windows does, and arguably more: MoltenVK
    // compiles every pipeline through the Metal shader compiler at startup, so
    // a cold cache blocks for minutes. Without the dialog the window is simply
    // frozen with nothing on screen.
    true;
#else
    false;
#endif

#if REX_PLATFORM_LINUX
std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string UnquoteOsReleaseValue(std::string value) {
  value = Trim(std::move(value));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    std::string unquoted;
    unquoted.reserve(value.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
      const char ch = value[i];
      if (escaped) {
        unquoted.push_back(ch);
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else {
        unquoted.push_back(ch);
      }
    }
    return unquoted;
  }
  return value;
}

std::optional<std::string> ReadOsReleaseValue(std::string_view key) {
  std::ifstream file("/etc/os-release");
  std::string line;
  while (std::getline(file, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    if (std::string_view(line.data(), equals) == key) {
      return UnquoteOsReleaseValue(line.substr(equals + 1));
    }
  }
  return std::nullopt;
}

std::string TruncateForLog(std::string value) {
  constexpr size_t kMaxLength = 512;
  if (value.size() <= kMaxLength) {
    return value;
  }
  value.resize(kMaxLength);
  value += "...";
  return value;
}

void LogEnvIfSet(const char* name) {
  const char* value = std::getenv(name);
  if (value && *value) {
    REXLOG_INFO("  {}={}", name, TruncateForLog(value));
  }
}

void LogLinuxRuntimeDiagnostics() {
  REXLOG_INFO("Linux runtime diagnostics:");

  const auto pretty_name = ReadOsReleaseValue("PRETTY_NAME");
  const auto id = ReadOsReleaseValue("ID");
  const auto version_id = ReadOsReleaseValue("VERSION_ID");
  if (pretty_name) {
    REXLOG_INFO("  OS: {}", *pretty_name);
  }
  if (id || version_id) {
    REXLOG_INFO("  OS ID: {} {}", id.value_or("unknown"), version_id.value_or(""));
  }

  utsname uts = {};
  if (uname(&uts) == 0) {
    REXLOG_INFO("  Kernel: {} {} {}", uts.sysname, uts.release, uts.machine);
  }
  REXLOG_INFO("  glibc: {}", gnu_get_libc_version());

#if defined(__clang__)
  REXLOG_INFO("  Compiler: clang {}", __clang_version__);
#elif defined(__GNUC__)
  REXLOG_INFO("  Compiler: GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif

  REXLOG_INFO("Linux session environment:");
  LogEnvIfSet("XDG_SESSION_TYPE");
  LogEnvIfSet("XDG_CURRENT_DESKTOP");
  LogEnvIfSet("DESKTOP_SESSION");
  LogEnvIfSet("DISPLAY");
  LogEnvIfSet("WAYLAND_DISPLAY");
  LogEnvIfSet("GDK_BACKEND");
  LogEnvIfSet("SDL_VIDEODRIVER");
  LogEnvIfSet("LD_LIBRARY_PATH");

  REXLOG_INFO("Steam runtime environment:");
  LogEnvIfSet("SteamAppId");
  LogEnvIfSet("SteamGameId");
  LogEnvIfSet("STEAM_COMPAT_APP_ID");
  LogEnvIfSet("STEAM_COMPAT_CLIENT_INSTALL_PATH");
  LogEnvIfSet("STEAM_COMPAT_DATA_PATH");
  LogEnvIfSet("STEAM_RUNTIME");
  LogEnvIfSet("STEAM_RUNTIME_LIBRARY_PATH");
  LogEnvIfSet("STEAM_RUNTIME_HEAVY");
  LogEnvIfSet("PRESSURE_VESSEL_RUNTIME");
  LogEnvIfSet("PRESSURE_VESSEL_APP_ID");
  LogEnvIfSet("container");

  REXLOG_INFO("Steam Deck / gamescope environment:");
  LogEnvIfSet("SteamDeck");
  LogEnvIfSet("STEAMOS");
  LogEnvIfSet("GAMESCOPE_WAYLAND_DISPLAY");
  LogEnvIfSet("GAMESCOPE_EXTERNAL_OVERLAY");
  LogEnvIfSet("ENABLE_GAMESCOPE_WSI");
  LogEnvIfSet("MESA_VK_WSI_PRESENT_MODE");
  LogEnvIfSet("RADV_PERFTEST");
  LogEnvIfSet("VK_ICD_FILENAMES");
  LogEnvIfSet("VK_DRIVER_FILES");
}

void StartForcedExitWatchdog(const char* reason) {
  std::thread([reason]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    REXLOG_WARN("{} watchdog exiting process after shutdown timeout", reason);
    std::_Exit(EXIT_SUCCESS);
  }).detach();
}
#endif

}  // namespace

REXCVAR_DEFINE_BOOL(advanced_settings_overlay_enabled, true, "UI/Advanced",
                    "Enable the developer cvar browser on F4");

REXCVAR_DEFINE_BOOL(show_fps_counter, false, "UI", "Show the guest FPS counter overlay")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::string_view ReXApp::GetBuildTitle() const {
  return REXGLUE_BUILD_TITLE;
}

std::string_view ReXApp::GetBuildStamp() const {
  return REXGLUE_BUILD_STAMP;
}

std::string ReXApp::GetWindowTitle() const {
  return std::string(GetName()) + " " + std::string(GetBuildTitle());
}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements());
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

#if REX_PLATFORM_MAC
  // Let the platform event loop run once before runtime setup. SDL creates the
  // Cocoa window synchronously, but the app may not visibly activate if guest
  // initialization starts before the first event-loop pass.
  app_context().CallInUIThreadDeferred([this, paths = std::move(*paths)]() mutable {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(paths)) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  });
#else
  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
#endif
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();
  auto config_path = exe_dir / (std::string(GetName()) + ".toml");

  // Load config before resolving cvar-backed paths such as game_data_root.
  if (std::filesystem::exists(config_path))
    rex::cvar::LoadConfig(config_path);

  toml::table config_table;
  bool has_config_table = false;
  if (std::filesystem::exists(config_path)) {
    try {
      config_table = toml::parse_file(config_path.string());
      has_config_table = true;
    } catch (const toml::parse_error&) {
      has_config_table = false;
    }
  }

  auto config_string = [&](std::string_view key) -> std::optional<std::string> {
    if (!has_config_table) {
      return std::nullopt;
    }
    if (auto value = config_table[key].value<std::string>()) {
      return *value;
    }
    return std::nullopt;
  };

  std::filesystem::path game_dir;
  if (auto config_value = config_string("game_data_root")) {
    game_dir = *config_value;
  }
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  if (auto config_value = config_string("user_data_root")) {
    user_dir = *config_value;
  }
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  }
  if (user_dir.empty()) {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  if (auto config_value = config_string("update_data_root")) {
    update_dir = *config_value;
  }
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  if (auto config_value = config_string("cache_path")) {
    cache_dir = *config_value;
  }
  std::string cache_path_cvar = REXCVAR_GET(cache_path);
  if (!cache_path_cvar.empty()) {
    cache_dir = cache_path_cvar;
  }
  if (cache_dir.empty()) {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  PathConfig path_config{game_dir,  user_dir,     update_dir,
                         cache_dir, metadata_dir, config_path};
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  metadata_root_ = path_config.metadata_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    log_config.log_dir = (exe_dir / "logs").string();
  }

  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_INFO("  Metadata root:  {}", metadata_root_.string());
  }
#if REX_PLATFORM_LINUX
  LogLinuxRuntimeDiagnostics();
#endif

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;

  runtime_ =
      std::make_unique<rex::Runtime>(paths.game_data_root, paths.user_data_root,
                                     paths.update_data_root, paths.cache_root, paths.metadata_root);
  runtime_->set_app_context(&app_context());
  config_.config_path = paths.config_path;

  // Recompiled guest code is linked into the host executable, while the kernel
  // imports live in rexruntime. Keep the exe-side clock state in sync with
  // Runtime::Setup so mftb and KeQueryPerformanceFrequency agree.
  rex::chrono::Clock::set_guest_tick_frequency(50000000);
  rex::chrono::Clock::set_guest_system_time_base(rex::chrono::Clock::QueryHostSystemTime());
  rex::chrono::Clock::set_guest_time_scalar(1.0);
  auto guest_tick_ratio = rex::chrono::Clock::guest_tick_ratio();
  REXLOG_INFO("Host guest clock initialized: frequency={} ratio={}/{}",
              rex::chrono::Clock::guest_tick_frequency(), guest_tick_ratio.first,
              guest_tick_ratio.second);

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([this]() {
        // dpour-fork 2026-07-03: use an EXPLICIT list of user-interactive
        // overlays (settings / console / achievements) instead of the
        // polymorphic HasInputCapturingDialog() poll. The polymorphic path
        // hit a real regression: achievement_toast_ + achievement_notification_
        // stay alive after boot and do NOT override WantsInputCapture()
        // (default returns true), so HasInputCapturingDialog was ALWAYS
        // true → SetActiveCallback returned false → input permanently
        // disabled. The explicit-list version dodges the false positive
        // entirely and only pauses real input when the user actually
        // opened an interactive overlay.
        return !(settings_overlay_ || console_overlay_ || achievements_overlay_);
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::SetupPresentation() {
#if REX_HAS_D3D12
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#elif REX_HAS_VULKAN
  config_.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#endif
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  window_->SetTitle(GetWindowTitle());

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
            window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        if (!frame_stats_provider_) {
          frame_stats_provider_ = [presenter]() {
            ui::Presenter::GuestFrameStats stats = presenter->GetGuestFrameStats();
            return ui::FrameStats{stats.frame_time_ms, stats.fps, stats.frame_count};
          };
        }
        auto update_guest_frame_stats_enabled = [this, presenter]() {
          presenter->SetGuestFrameStatsEnabled(fps_overlay_ != nullptr || debug_overlay_ != nullptr);
        };
        rex::ui::RegisterBind("bind_fps_counter", "F2", "Toggle FPS counter", [this, presenter] {
          if (fps_overlay_) {
            fps_overlay_.reset();
          } else {
            fps_overlay_ =
                std::make_unique<ui::FpsOverlayDialog>(imgui_drawer_.get(), presenter);
          }
          REXCVAR_SET(show_fps_counter, fps_overlay_ != nullptr);
          presenter->SetGuestFrameStatsEnabled(fps_overlay_ != nullptr || debug_overlay_ != nullptr);
          if (!config_path_.empty()) {
            rex::cvar::SaveConfigValues(config_path_, {"show_fps_counter"});
          }
        });
        if (REXCVAR_GET(show_fps_counter)) {
          fps_overlay_ = std::make_unique<ui::FpsOverlayDialog>(imgui_drawer_.get(), presenter);
        }
        // PSO compile indicator: pops in during gameplay background compiles
        // and auto-hides ~2 s after the queue empties. Allocated once at
        // startup; its OnDraw self-gates on the cvar so toggling at runtime
        // is free.
        if (!pso_compile_indicator_overlay_) {
          pso_compile_indicator_overlay_ =
              std::make_unique<ui::PsoCompileIndicatorDialog>(imgui_drawer_.get());
        }
        rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
          if (debug_overlay_) {
            debug_overlay_.reset();
          } else {
            debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(
                imgui_drawer_.get(), frame_stats_provider_, GetBuildStamp());
          }
          auto* graphics_system = static_cast<rex::graphics::GraphicsSystem*>(config_.graphics.get());
          if (graphics_system && graphics_system->presenter()) {
            graphics_system->presenter()->SetGuestFrameStatsEnabled(
                fps_overlay_ != nullptr || debug_overlay_ != nullptr);
          }
        });
        update_guest_frame_stats_enabled();
        rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
          if (console_overlay_) {
            console_overlay_.reset();
          } else {
            console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
          }
        });
        rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
          if (!REXCVAR_GET(advanced_settings_overlay_enabled)) {
            return;
          }
          if (settings_overlay_) {
            settings_overlay_.reset();
          } else {
            settings_overlay_ =
                std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
          }
        });
        rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements overlay", [this] {
          if (achievements_overlay_) {
            achievements_overlay_.reset();
          } else {
            achievements_overlay_ = CreateAchievementsOverlay();
          }
        });

        OnCreateDialogs(imgui_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  }

  return true;
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    if (!achievement_notification_) {
      achievement_notification_ =
          std::shared_ptr<ui::AchievementNotificationDialog>(CreateAchievementNotificationDialog());
    }
    if (achievement_notification_ && achievement_notification_listener_ == 0 && runtime_ &&
        runtime_->kernel_state()) {
      std::weak_ptr<ui::AchievementNotificationDialog> notification = achievement_notification_;
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [notification](const rex::system::AchievementEvent& event) {
            if (auto dialog = notification.lock()) {
              dialog->Push(event);
            }
          });
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }
#if REX_PLATFORM_MAC
    main_thread_ = main_thread;
#endif

    auto* graphics_system =
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system());
    const bool has_shader_storage =
        graphics_system && !runtime_->cache_root().empty() &&
        runtime_->kernel_state()->title_id() != 0;

    if (!has_shader_storage) {
      FinishModuleLaunch(std::move(main_thread));
      return;
    }

    const uint32_t title_id = runtime_->kernel_state()->title_id();
    REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);

    // Drive the blocking init off the UI thread when an ImGui overlay is
    // available so the user sees a "Compiling shaders X of Y" progress dialog
    // while the D3D12 driver chews through the warmed-cache PSO list. Without
    // this the window appears frozen for many seconds on cold launches with
    // large caches (Downpour ROV path: ~hundreds of PSOs).
    if (kBlockShaderStorageStartup && imgui_drawer_) {
      auto cache_root = runtime_->cache_root();
      // The dialog adds itself to imgui_drawer_'s dialog list in its ctor and
      // Close()s itself once `finished` flips — ownership is the dialog list.
      auto main_thread_shared =
          std::make_shared<system::object_ref<system::XThread>>(std::move(main_thread));
      new rex::ui::ShaderCompileDialog(
          imgui_drawer_.get(), graphics_system, "Preparing shaders",
#if REX_PLATFORM_MAC
          "Warming the Metal pipeline cache for the current render path. "
          "This only runs in full once per cache; later launches are fast.",
#else
          "Warming the D3D12 pipeline cache for the current render path. "
          "This only runs in full once per cache; later launches are fast.",
#endif
          [this, main_thread_shared]() mutable {
            // Already on UI thread (dialog renders here).
            FinishModuleLaunch(std::move(*main_thread_shared));
          });

      // Background worker: the SDK's blocking InitializeShaderStorage internally
      // dispatches to the CP thread via a fence; the worker just sits on the
      // fence so the UI thread keeps pumping frames + drawing the dialog.
      std::thread([graphics_system, cache_root = std::move(cache_root), title_id]() {
        graphics_system->InitializeShaderStorage(cache_root, title_id, true);
      }).detach();
      return;
    }

    // Headless / non-overlay fallback: keep the legacy direct blocking init.
    graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id,
                                             kBlockShaderStorageStartup);
    FinishModuleLaunch(std::move(main_thread));
  });
}

void ReXApp::FinishModuleLaunch(system::object_ref<system::XThread> main_thread) {
  OnPostLaunchModule(main_thread.get());
  main_thread->Resume();

  module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
    main_thread->Wait(0, 0, 0, nullptr);
    OnGuestThreadExit(main_thread.get());
    REXLOG_INFO("Execution complete");
    if (!shutting_down_.load(std::memory_order_acquire)) {
      app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
    }
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    // The acquire wizard's "Start Game" button calls this callback FROM INSIDE
    // its ImGui::Draw(). ConstructRuntime/LaunchModule take seconds (XEX load,
    // renderer init, kernel boot) — running them inline blocks the UI thread
    // mid-frame and the window appears to hang until they finish. Defer to the
    // next UI-thread event-loop iteration so the wizard's frame completes and
    // its dialog tears down cleanly before heavy work begins. Mirrors the
    // existing Mac startup path in OnInitialize.
    app_context().CallInUIThreadDeferred(
        [this, paths = std::move(paths)]() mutable {
          if (shutting_down_.load(std::memory_order_acquire))
            return;
          if (!ConstructRuntime(std::move(paths))) {
            app_context().QuitFromUIThread();
            return;
          }
          LaunchModule();
        });
  };
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
#if REX_PLATFORM_LINUX
  StartForcedExitWatchdog("Linux window close");
#endif
  shutting_down_.store(true, std::memory_order_release);
#if REX_PLATFORM_MAC
  if (main_thread_ && main_thread_->is_running()) {
    main_thread_->Terminate(0);
  }
#endif
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // Notify subclass before cleanup
  OnShutdown();

#if REX_PLATFORM_MAC
  shutting_down_.store(true, std::memory_order_release);
  if (main_thread_ && main_thread_->is_running()) {
    main_thread_->Terminate(0);
  }
#endif

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_achievements");

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
#if REX_PLATFORM_MAC
  main_thread_ = nullptr;
#endif
  // Input drivers may still be registered as window listeners. Detach them
  // before destroying the SDL window so their backend state is released in
  // listener order.
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->Shutdown();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
