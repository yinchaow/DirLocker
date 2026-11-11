#pragma once

#include <string>
#include <vector>

namespace dirlocker {

struct PathCheckResult {
    bool ok = false;
    std::string canonical_path;
    std::string error;
};

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
#endif

// Normalizes path syntax cleanly (slashes, trailing slash removal) without requiring existence.
std::string normalize_path_lexical(const std::string& path);

// Compares two canonical paths appropriately for the host OS (case-insensitive on Windows).
bool paths_equal(const std::string& a, const std::string& b);

// Returns true if 'child' is inside 'parent' (or equal to 'parent').
bool is_same_or_subpath(const std::string& child, const std::string& parent);

// Returns true if two paths overlap (equal, or one is ancestor of the other).
bool paths_overlap(const std::string& a, const std::string& b);

// Checks whether a path is a protected OS/system/user-critical directory that must NEVER be locked.
// user_home can be passed explicitly (e.g., from getpwuid on Linux daemon when running as root for a uid).
bool is_protected_system_path(const std::string& canonical_path,
                              const std::string& user_home = "",
                              const std::vector<std::string>& extra_protected = {},
                              std::string* reason = nullptr);

// Validates that raw_path exists, is a real non-symlink non-mountpoint directory,
// is not in the protected blacklist, and does not overlap with existing_paths.
PathCheckResult validate_lockable_directory(
    const std::string& raw_path,
    const std::vector<std::string>& existing_paths,
    const std::string& user_home = "",
    const std::vector<std::string>& extra_protected = {});

// Canonicalizes an existing directory path without checking lockability (for unlock/remove).
bool canonicalize_existing_dir(const std::string& raw_path, std::string& out, std::string* err = nullptr);

}  // namespace dirlocker

