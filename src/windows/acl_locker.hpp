#pragma once

#include "../core/state_store.hpp"

#include <string>
#include <vector>

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

struct WinHealthItem {
    enum class IssueKind {
        kOk = 0,
        kMissingDlockStub,
        kMissingCompanionStub,
        kStaleUnlockedWithStub,
        kExternallyRestored,
        kLeftoverVaultAndDir,
        kVaultMissing,
        kOrphanVaultDir,
        kMissingOrigDirUnlocked,
        kUnmountedDrive
    };

    std::string entry_id;
    std::string display_path;
    std::string vault_path;
    IssueKind kind = IssueKind::kOk;
    std::string summary;
    std::string repair_hint;
    bool auto_repairable = false;
    bool allow_remove_record = false;
};

struct WinHealthReport {
    int total_entries = 0;
    int ok_count = 0;
    int repairable_count = 0;
    int manual_count = 0;
    int unmounted_count = 0;
    int orphan_vault_count = 0;
    std::vector<WinHealthItem> items;
};

class StateStore;

// Inspects open file handles (Restart Manager) and Explorer windows inside target_path.
std::vector<std::string> win_detect_blocking_processes(const std::string& target_path);

// Builds companion stub path e.g. "D:\目录（已锁定，点此解锁）.dlock" for in-place ACL mode.
std::string win_make_companion_dlock_path(const std::string& dir_path);

// Primary Windows lock: moves real folder atomically on same drive to hidden ACL-locked vault
// and places a delete-protected .dlock entry at the original path so double-clicking pops up
// a single 目录密匣 password prompt. Falls back to in-place ACL + companion .dlock only when enabled.
bool win_smart_lock(DirEntry& entry, const std::string& self_exe, const std::string& state_dir,
                    bool allow_in_place_fallback, std::string* err = nullptr);

// Unlocks either a vault-backed entry or a legacy/fallback in-place ACL entry.
bool win_smart_unlock(DirEntry& entry, std::string* err = nullptr);

// Parses a .dlock stub file to retrieve its entry ID and recorded original path.
bool win_read_dlock_file(const std::string& dlock_path, std::string& out_id, std::string& out_orig_path);

// Recreates a missing .dlock stub (or companion .dlock for in-place ACL) when locked data is intact.
bool win_repair_locked_entry_stub(DirEntry& entry);

// Attempts background migration of an in-place ACL entry into the vault once handles close.
bool win_try_upgrade_in_place_entry(DirEntry& entry, const std::string& self_exe,
                                    const std::string& state_dir, bool* upgraded,
                                    std::string* err = nullptr);

// Non-destructive health audit and safe auto-repair across StateStore + vault roots.
WinHealthReport win_diagnose_state(const StateStore& store, const std::string& state_dir);
bool win_repair_state(StateStore& store, const std::string& state_dir, bool clean_dead_records,
                      int* repaired_count, std::vector<std::string>* details,
                      std::string* err = nullptr);

}  // namespace dirlocker
