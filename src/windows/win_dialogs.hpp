#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace dirlocker {

// Creates crisp Microsoft YaHei UI font scaled to DPI.
HFONT create_ui_font(int dpi, int pt = 9, bool bold = false);

// Returns currently open modal dialog window handle if any, else nullptr.
HWND win_get_active_modal_dialog();

// Brings an external/topmost popup dialog cleanly to foreground.
void win_bring_window_to_front(HWND hwnd, bool topmost = true);

// Helper for DPI scaling.
int get_window_dpi(HWND hwnd);
int scale_dpi(int px, int dpi);

// Opens native modern folder picker dialog. Returns empty string if cancelled.
std::string win_pick_folder(HWND parent);

bool win_show_master_password_dialog(HWND parent, bool require_old,
                                     std::string& old_pw, std::string& new_pw);

bool win_show_lock_dir_dialog(HWND parent, const std::string& target_summary,
                              bool& use_custom, std::string& custom_pw);

bool win_show_unlock_dir_dialog(HWND parent, const std::string& dir_path,
                                bool use_custom, bool show_open_option,
                                const std::wstring& title_override,
                                const std::wstring& subtitle_override,
                                std::string& password, bool& open_after);

bool win_show_verify_master_dialog(HWND parent, const std::wstring& title,
                                   const std::wstring& prompt, std::string& password);

bool win_show_change_dir_pw_dialog(HWND parent, const std::string& dir_path,
                                     bool currently_custom,
                                     std::string& cur_pw, std::string& new_pw);

bool win_show_edit_custom_pw_dialog(HWND parent, const std::string& dir_path,
                                    std::string& cur_pw, std::string& new_pw);

bool win_show_settings_dialog(HWND parent, int& auto_relock_minutes,
                              bool& relock_on_screen_lock, bool hotkey_ok,
                              std::string& master_pw);

void win_show_recovery_dialog(HWND parent);

void win_show_about_dialog(HWND parent);

}  // namespace dirlocker
