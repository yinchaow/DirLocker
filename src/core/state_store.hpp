#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dirlocker {

struct DirEntry {
    std::string id;
    std::string path;
    bool locked = false;
    bool use_custom_password = false;
    std::string custom_hash;

    // Linux metadata backup for exact lossless restore
    uint32_t orig_mode = 0755;
    uint32_t orig_uid = 0;
    uint32_t orig_gid = 0;
    bool set_immutable = false;

    // Vault/stub paths (used on both Windows and Linux vault mode)
    std::string win_sid;
    std::string vault_path;
    std::string stub_path;
    bool in_place_acl = false;
    uint32_t win_orig_attrs = 0;

    int64_t created_at = 0;
    int64_t last_unlocked_at = 0;
};

struct AppSettings {
    int auto_relock_minutes = 5;          // 0 means disabled
    bool require_master_on_open = false;  // Require master password before showing main window
    bool allow_in_place_fallback = false; // Windows: allow in-place ACL fallback if occupied
    std::string hotkey = "Ctrl+Alt+L";
};

class StateStore {
public:
    explicit StateStore(std::string file_path);

    const std::string& file_path() const { return file_path_; }

    bool load(std::string* err = nullptr);
    bool save(std::string* err = nullptr) const;

    bool has_master_password() const { return !master_hash_.empty(); }
    bool set_master_password(const std::string& password);
    bool verify_master_password(const std::string& password) const;

    bool verify_entry_password(const DirEntry& entry, const std::string& password) const;

    const AppSettings& settings() const { return settings_; }
    AppSettings& settings_mut() { return settings_; }

    const std::vector<DirEntry>& entries() const { return entries_; }
    std::vector<DirEntry>& entries_mut() { return entries_; }

    DirEntry* find_by_path(const std::string& canonical_path);
    const DirEntry* find_by_path(const std::string& canonical_path) const;
    DirEntry* find_by_id(const std::string& id);

    std::vector<std::string> all_paths() const;

    static std::string generate_id();
    static int64_t now_epoch();

private:
    std::string file_path_;
    std::string master_hash_;
    AppSettings settings_;
    std::vector<DirEntry> entries_;
};

}  // namespace dirlocker

