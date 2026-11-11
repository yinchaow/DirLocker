#pragma once

#include <string>

namespace dirlocker {

// Returns full path of the running dirlocker.exe in UTF-8.
std::string get_self_exe_path();

// Returns %LOCALAPPDATA%\DirLocker\state.json in UTF-8.
std::string get_windows_state_path();

// Returns %LOCALAPPDATA%\Programs\DirLocker\dirlocker.exe in UTF-8.
std::string get_default_install_exe_path();

// Ensures HKCU .dlock file type association (NeverShowExt, no shortcut arrow, custom folder-lock icon).
bool win_ensure_dlock_association(const std::string& exe_path);

// Registers Explorer right-click menu, .dlock association, and optional login autostart / Start Menu under HKCU.
bool win_install_integration(const std::string& exe_path,
                             bool with_autostart = true,
                             bool with_start_menu = true,
                             std::string* err = nullptr);

// Removes Explorer right-click menu, .dlock association, login autostart, and Start Menu shortcut under HKCU.
bool win_uninstall_integration(std::string* err = nullptr);

}  // namespace dirlocker
