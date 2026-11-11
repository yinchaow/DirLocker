#pragma once

#include "../core/state_store.hpp"

#include <string>

namespace dirlocker {

// Returns the current user's SID string (e.g. "S-1-5-21-...") or empty on failure.
std::string get_current_user_sid_string();

// Applies a surgical non-recursive DENY ACE on the top-level directory only.
// Does NOT modify any files or subdirectories inside the folder.
bool win_lock_directory(const std::string& path, std::string& used_sid, std::string* err = nullptr);

// Removes the surgical DENY ACE added by win_lock_directory, restoring original permissions.
bool win_unlock_directory(const std::string& path, const std::string& used_sid, std::string* err = nullptr);

// Returns true if our exact DENY ACE is currently present on the directory.
bool win_is_directory_locked(const std::string& path, const std::string& used_sid);

// Primary Windows lock: moves real folder atomically on same drive to hidden ACL-locked vault
// and places a delete-protected .dlock entry at the original path so double-clicking pops up
// a single 目录密匣 password prompt. Optionally falls back to in-place ACL if folder is open.
bool win_smart_lock(DirEntry& entry, const std::string& self_exe, const std::string& state_dir,
                    bool allow_in_place_fallback, std::string* err = nullptr);

// Unlocks either a vault-backed entry or a legacy/fallback in-place ACL entry.
bool win_smart_unlock(DirEntry& entry, std::string* err = nullptr);

// Parses a .dlock stub file to retrieve its entry ID and recorded original path.
bool win_read_dlock_file(const std::string& dlock_path, std::string& out_id, std::string& out_orig_path);

// Recreates a missing .dlock stub (or upgrades legacy .lnk to .dlock) when vault data is intact.
bool win_repair_locked_entry_stub(const DirEntry& entry);

}  // namespace dirlocker
