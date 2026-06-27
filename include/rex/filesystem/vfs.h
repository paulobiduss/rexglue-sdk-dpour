/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/cvar.h>
#include <rex/thread/mutex.h>

REXCVAR_DECLARE(bool, allow_game_relative_writes);

namespace rex::filesystem {

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  bool RegisterDevice(std::unique_ptr<Device> device);
  bool UnregisterDevice(const std::string_view path);

  bool RegisterSymbolicLink(const std::string_view path, const std::string_view target);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  Entry* ResolvePath(const std::string_view path);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition, uint32_t desired_access,
                    bool is_directory, bool is_non_directory, File** out_file,
                    FileAction* out_action);

 private:
  rex::thread::global_critical_region global_critical_region_;
  std::vector<std::unique_ptr<Device>> devices_;
  std::unordered_map<std::string, std::string> symlinks_;

  // Negative-result cache: paths that ResolvePath previously returned nullptr
  // for. UE3/Downpour probes hundreds of nonexistent files per second (jpn_SF
  // localization, temp:\T_PART_VISIBILITY, etc.). Each miss triggered a full
  // case-insensitive directory listing in HostPathDevice::ResolvePath, which
  // caused 300-2000 ms frame stalls. Caching the "not found" verdict turns
  // repeated probes into an O(1) hash lookup. Cleared on RegisterDevice /
  // RegisterSymbolicLink / CreatePath so a path that becomes valid stops
  // returning stale-negative.
  std::unordered_set<std::string> resolve_negative_cache_;
  // Sized so a typical session never evicts. Each entry is ~50 bytes string;
  // 16k entries = ~800 KB worst case.
  static constexpr size_t kNegativeCacheCapacity = 16384;

  bool ResolveSymbolicLink(const std::string_view path, std::string& result);
};

}  // namespace rex::filesystem
