#include "../core/crypto.hpp"
#include "../core/json_mini.hpp"
#include "../core/path_guard.hpp"
#include "../core/state_store.hpp"
#include "../core/version.hpp"
#include "acl_locker.hpp"
#include "resource.h"
#include "win_dialogs.hpp"
#include "win_registry.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dirlocker {

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_IPC = WM_APP + 2;
constexpr UINT_PTR kIdleTimerId = 1;
constexpr int kHotkeyId = 1;

enum ControlId {
    IDC_BTN_ADD = 101,
    IDC_BTN_LOCK_ALL,
    IDC_SEARCH_EDIT,
    IDC_BTN_MENU_SETTINGS,
    IDC_BTN_MENU_HELP,
    IDC_BTN_SETTINGS,
    IDC_BTN_MASTER_PW,
    IDC_BTN_DIR_PW,
    IDC_BTN_EDIT_CUSTOM_PW,
    IDC_BTN_REMOVE,
    IDC_BTN_RECOVERY,
    IDC_BTN_ABOUT,
    IDC_LIST_DIRS,
    IDC_STATUS_LABEL,
    IDC_POLICY_LABEL,
    IDM_TRAY_OPEN = 201,
    IDM_TRAY_LOCK_ALL,
    IDM_TRAY_SETTINGS,
    IDM_TRAY_MASTER_PW,
    IDM_TRAY_ABOUT,
    IDM_TRAY_EXIT,
    IDM_ROW_UNLOCK_OR_OPEN = 301,
    IDM_ROW_LOCK,
    IDM_ROW_DIR_PW,
    IDM_ROW_EDIT_CUSTOM_PW,
    IDM_ROW_REMOVE
};

struct IpcCommand {
    std::string action;
    std::string path;
};

struct WinApp {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND empty_overlay = nullptr;
    HWND search_edit = nullptr;
    HWND status_label = nullptr;
    HWND policy_label = nullptr;
    HWND list_tip = nullptr;
    HWND hovered_btn = nullptr;
    HFONT font = nullptr;
    HFONT bold_font = nullptr;
    HFONT small_font = nullptr;
    HBRUSH bg_brush = nullptr;
    HBRUSH card_brush = nullptr;
    HICON icon = nullptr;
    HICON folder_lock_icon = nullptr;
    HICON folder_plain_icon = nullptr;
    RECT search_rc{};
    RECT list_card_rc{};
    bool search_focused = false;
    int status_line_y = 0;
    int dpi = 96;
    UINT taskbar_created_msg = 0;
    UINT quit_silent_msg = 0;

    WNDPROC orig_list_proc = nullptr;
    int hover_row = -1;
    int hover_col = -1;
    int hover_slot = 0;
    std::wstring tip_text;
    ULONG_PTR gdiplus_token = 0;

    std::unique_ptr<StateStore> store;
    std::vector<size_t> visible_entry_indices;
    std::string self_exe;
    std::string state_dir;
    std::vector<std::string> extra_protected;

    bool tray_added = false;
    bool hotkey_ok = false;
    bool idle_triggered = false;
    bool really_quit = false;

    std::wstring pipe_name;
    std::atomic<bool> pipe_running{false};
    HANDLE pipe_thread = nullptr;
};

std::string parent_dir_of(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    if (p == std::string::npos) return {};
    return path.substr(0, p);
}

std::wstring build_pipe_name() {
    std::string sid = get_current_user_sid_string();
    if (sid.empty()) sid = "default";
    return L"\\\\.\\pipe\\DirLockerIpc_" + utf8_to_wide(sid);
}

bool try_forward_to_running_instance(const std::wstring& pipe_name,
                                     const std::string& action,
                                     const std::string& path) {
    AllowSetForegroundWindow(ASFW_ANY);
    HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeW(pipe_name.c_str(), 1500)) {
                pipe = CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            }
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    JsonValue msg = JsonValue::object();
    msg.set("action", action);
    msg.set("path", path);
    std::string raw = msg.dump(-1) + "\n";

    DWORD written = 0;
    WriteFile(pipe, raw.data(), static_cast<DWORD>(raw.size()), &written, nullptr);
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return true;
}

void add_or_update_tray(WinApp& app, bool modify = false) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = app.icon;

    int locked = 0;
    int unlocked = 0;
    for (const auto& e : app.store->entries()) {
        if (e.locked) ++locked;
        else ++unlocked;
    }
    std::wstring tip = L"目录密匣（已锁定 " + std::to_wstring(locked) +
                       L" / 已解锁 " + std::to_wstring(unlocked) + L"）";
    wcsncpy(nid.szTip, tip.c_str(), 127);

    Shell_NotifyIconW(modify && app.tray_added ? NIM_MODIFY : NIM_ADD, &nid);
    app.tray_added = true;
}

void remove_tray(WinApp& app) {
    if (!app.tray_added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    app.tray_added = false;
}

void show_tray_balloon(WinApp& app, const std::wstring& title, const std::wstring& body) {
    if (!app.tray_added) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = app.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(nid.szInfo, body.c_str(), 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring get_window_text_w_main(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return {};
    std::wstring buf(static_cast<size_t>(len + 1), L'\0');
    int got = GetWindowTextW(h, &buf[0], len + 1);
    buf.resize(static_cast<size_t>(got > 0 ? got : 0));
    return buf;
}

std::wstring to_lower_w(std::wstring s) {
    if (!s.empty()) {
        CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    }
    return s;
}

std::string basename_utf8(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return path;
    std::string b = path.substr(pos + 1);
    return b.empty() ? path : b;
}

int get_selected_row(WinApp& app);
void adjust_list_columns(WinApp& app);
void update_empty_overlay_layout(WinApp& app);

void rebuild_list_view(WinApp& app) {
    std::string prev_path;
    int prev_idx = get_selected_row(app);
    if (prev_idx >= 0 && prev_idx < static_cast<int>(app.store->entries().size())) {
        prev_path = app.store->entries()[static_cast<size_t>(prev_idx)].path;
    }

    std::wstring filter = app.search_edit ? to_lower_w(get_window_text_w_main(app.search_edit)) : L"";
    while (!filter.empty() && filter.back() == L' ') filter.pop_back();
    size_t lead = 0;
    while (lead < filter.size() && filter[lead] == L' ') ++lead;
    if (lead > 0) filter = filter.substr(lead);

    SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.list);
    app.visible_entry_indices.clear();

    int locked_cnt = 0;
    int unlocked_cnt = 0;
    int row_to_select = -1;
    const auto& entries = app.store->entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.locked) ++locked_cnt;
        else ++unlocked_cnt;

        std::wstring wp = utf8_to_wide(e.path);
        if (!filter.empty()) {
            std::wstring lw = to_lower_w(wp);
            if (lw.find(filter) == std::wstring::npos) continue;
        }

        int vis_row = static_cast<int>(app.visible_entry_indices.size());
        if (!prev_path.empty() && paths_equal(e.path, prev_path)) {
            row_to_select = vis_row;
        }
        app.visible_entry_indices.push_back(i);

        std::wstring st = e.locked ? L"已锁定" : L"已解锁";
        std::wstring mode = e.use_custom_password ? L"独立密码" : L"主密码";
        std::wstring op = e.locked ? L"解锁" : L"锁定";

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = vis_row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(wp.c_str());
        ListView_InsertItem(app.list, &item);
        ListView_SetItemText(app.list, vis_row, 1, const_cast<LPWSTR>(st.c_str()));
        ListView_SetItemText(app.list, vis_row, 2, const_cast<LPWSTR>(mode.c_str()));
        ListView_SetItemText(app.list, vis_row, 3, const_cast<LPWSTR>(op.c_str()));
    }

    if (row_to_select >= 0) {
        ListView_SetItemState(app.list, row_to_select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
    adjust_list_columns(app);
    update_empty_overlay_layout(app);
    InvalidateRect(app.list, nullptr, TRUE);

    std::wstring summary = L"受管目录 " + std::to_wstring(entries.size()) +
                           L" 个 · 已锁定 " + std::to_wstring(locked_cnt) +
                           L" · 已解锁 " + std::to_wstring(unlocked_cnt);
    if (!filter.empty()) {
        summary += L"（筛选显示 " + std::to_wstring(app.visible_entry_indices.size()) + L" 项）";
    }
    SetWindowTextW(app.status_label, summary.c_str());

    if (app.policy_label) {
        int mins = app.store->settings().auto_relock_minutes;
        std::wstring hk = app.hotkey_ok ? L"热键 Ctrl+Alt+L 已就绪" : L"托盘保护运行中";
        std::wstring idle = mins > 0
            ? (L"闲置 " + std::to_wstring(mins) + L" 分钟自动回锁")
            : L"闲置回锁已关闭";
        std::wstring policy = hk + L"  |  " + idle;
        SetWindowTextW(app.policy_label, policy.c_str());
    }

    add_or_update_tray(app, true);
}

void refresh_ui(WinApp& app) {
    std::string err;
    app.store->load(&err);
    for (const auto& e : app.store->entries()) {
        win_repair_locked_entry_stub(e);
    }
    rebuild_list_view(app);
}

void show_modal_msg(HWND owner, const std::wstring& text, const std::wstring& title, UINT icon) {
    UINT flags = icon | (owner ? 0 : (MB_TOPMOST | MB_SETFOREGROUND));
    MessageBoxW(owner, text.c_str(), title.c_str(), flags);
}

void show_main_window(WinApp& app) {
    win_bring_window_to_front(app.hwnd, false);
    refresh_ui(app);
}

bool ensure_master_password(WinApp& app, HWND owner) {
    if (app.store->has_master_password()) return true;
    std::string old_pw, new_pw;
    if (!win_show_master_password_dialog(owner, false, old_pw, new_pw)) return false;

    if (!app.store->set_master_password(new_pw)) {
        MessageBoxW(owner, L"生成主密码哈希失败。", L"错误", MB_ICONERROR);
        return false;
    }
    std::string err;
    if (!app.store->save(&err)) {
        MessageBoxW(owner, utf8_to_wide("保存状态文件失败: " + err).c_str(), L"错误", MB_ICONERROR);
        return false;
    }
    refresh_ui(app);
    return true;
}

void open_in_explorer(const std::string& path) {
    std::wstring wp = utf8_to_wide(path);
    ShellExecuteW(nullptr, L"open", wp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool lock_existing_entry(WinApp& app, DirEntry& entry, bool allow_fallback = false,
                         std::string* err_out = nullptr) {
    if (entry.locked) return true;
    std::string err;
    if (!win_smart_lock(entry, app.self_exe, app.state_dir, allow_fallback, &err)) {
        if (err_out) *err_out = err;
        return false;
    }
    if (!app.store->save(&err)) {
        if (err_out) *err_out = err;
        return false;
    }
    return true;
}

bool lock_new_paths_batch(WinApp& app, HWND owner, const std::vector<std::string>& raw_paths) {
    if (raw_paths.empty()) return false;
    if (!ensure_master_password(app, owner)) return false;

    std::vector<std::string> candidate_paths;
    std::vector<std::string> virtual_existing = app.store->all_paths();
    std::vector<std::string> errors;
    int relocked = 0;

    for (const auto& raw : raw_paths) {
        std::string canonical;
        std::string cerr;
        if (!canonicalize_existing_dir(raw, canonical, &cerr)) {
            errors.push_back(raw + " (" + cerr + ")");
            continue;
        }
        if (DirEntry* ex = app.store->find_by_path(canonical)) {
            if (ex->locked) {
                errors.push_back(canonical + " (已处于锁定状态)");
            } else if (lock_existing_entry(app, *ex, false, &cerr)) {
                ++relocked;
            } else {
                errors.push_back(canonical + " (" + cerr + ")");
            }
            continue;
        }
        auto check = validate_lockable_directory(canonical, virtual_existing, "", app.extra_protected);
        if (!check.ok) {
            errors.push_back(canonical + " (" + check.error + ")");
            continue;
        }
        virtual_existing.push_back(check.canonical_path);
        candidate_paths.push_back(check.canonical_path);
    }

    int newly_locked = 0;
    if (!candidate_paths.empty()) {
        std::string summary;
        if (candidate_paths.size() == 1) {
            summary = "目标目录：" + candidate_paths.front();
        } else {
            summary = "批量锁定 " + std::to_string(candidate_paths.size()) + " 个目录：\n";
            size_t show = candidate_paths.size() < 3 ? candidate_paths.size() : 3;
            for (size_t i = 0; i < show; ++i) {
                summary += "• " + candidate_paths[i] + "\n";
            }
            if (candidate_paths.size() > 3) {
                summary += "等共 " + std::to_string(candidate_paths.size()) + " 个目录";
            }
        }

        bool use_custom = false;
        std::string custom_pw;
        if (!win_show_lock_dir_dialog(owner, summary, use_custom, custom_pw)) {
            if (relocked > 0) refresh_ui(app);
            return relocked > 0;
        }

        std::string custom_hash;
        if (use_custom) {
            custom_hash = hash_password(custom_pw);
            if (custom_hash.empty()) {
                MessageBoxW(owner, L"生成独立密码哈希失败。", L"错误", MB_ICONERROR);
                return false;
            }
        }

        for (const auto& cp : candidate_paths) {
            DirEntry entry;
            entry.id = StateStore::generate_id();
            entry.path = cp;
            entry.use_custom_password = use_custom;
            entry.custom_hash = custom_hash;
            entry.created_at = StateStore::now_epoch();
            std::string err;
            if (!win_smart_lock(entry, app.self_exe, app.state_dir, false, &err)) {
                errors.push_back(cp + " (" + err + ")");
                continue;
            }
            app.store->entries_mut().push_back(std::move(entry));
            ++newly_locked;
        }
        std::string save_err;
        if (newly_locked > 0 && !app.store->save(&save_err)) {
            show_modal_msg(owner, utf8_to_wide("保存状态文件失败: " + save_err), L"错误", MB_ICONERROR);
            return false;
        }
    }

    refresh_ui(app);
    int total_locked = newly_locked + relocked;
    if (total_locked > 0) {
        if (total_locked == 1 && raw_paths.size() == 1 && candidate_paths.size() == 1) {
            show_tray_balloon(app, L"目录密匣", L"已锁定目录：\n" + utf8_to_wide(candidate_paths.front()));
        } else {
            show_tray_balloon(app, L"目录密匣",
                              L"成功锁定 " + std::to_wstring(total_locked) + L" 个目录");
        }
    }
    if (!errors.empty()) {
        std::string msg = "以下目录未能锁定：\n";
        for (size_t i = 0; i < errors.size() && i < 6; ++i) {
            msg += "• " + errors[i] + "\n";
        }
        if (errors.size() > 6) {
            msg += "等共 " + std::to_string(errors.size()) + " 项";
        }
        show_modal_msg(owner, utf8_to_wide(msg),
                       total_locked > 0 ? L"部分目录已跳过" : L"无法锁定", MB_ICONWARNING);
    }
    return total_locked > 0;
}

bool lock_new_path_flow(WinApp& app, HWND owner, const std::string& raw_path) {
    return lock_new_paths_batch(app, owner, {raw_path});
}

bool unlock_path_flow(WinApp& app, HWND owner, DirEntry& entry, bool from_explorer = false) {
    std::wstring title = L"目录已加密码";
    std::wstring subtitle = from_explorer
        ? L"该目录已加密码保护，请输入密码解锁（取消则拒绝进入）："
        : L"该目录已加密码保护，请输入密码解锁：";
    while (true) {
        std::string pw;
        bool open_after = true;
        if (!win_show_unlock_dir_dialog(owner, entry.path, entry.use_custom_password,
                                        true, title, subtitle, pw, open_after)) {
            return false;
        }
        if (!app.store->verify_entry_password(entry, pw)) {
            show_modal_msg(owner, L"密码错误，请重试。", L"验证失败", MB_ICONWARNING);
            continue;
        }
        std::string err;
        if (!win_smart_unlock(entry, &err)) {
            show_modal_msg(owner, utf8_to_wide(err), L"解锁失败", MB_ICONERROR);
            return false;
        }
        if (!app.store->save(&err)) {
            show_modal_msg(owner, utf8_to_wide("保存状态文件失败: " + err), L"错误", MB_ICONERROR);
            return false;
        }
        std::string path_copy = entry.path;
        refresh_ui(app);
        if (open_after) open_in_explorer(path_copy);
        return true;
    }
}

void lock_all_dirs(WinApp& app, const std::wstring& reason, bool notify_zero) {
    int count = 0;
    for (auto& e : app.store->entries_mut()) {
        if (e.locked) continue;
        std::string err;
        if (win_smart_lock(e, app.self_exe, app.state_dir, true, &err)) {
            ++count;
        }
    }
    if (count > 0) {
        app.store->save(nullptr);
    }
    refresh_ui(app);
    if (count > 0 || notify_zero) {
        show_tray_balloon(app, L"目录密匣",
                          reason + L"：共锁定 " + std::to_wstring(count) + L" 个目录");
    }
}

void handle_command(WinApp& app, const std::string& action, const std::string& raw_path) {
    app.store->load(nullptr);
    if (action == "quit-silent") {
        app.really_quit = true;
        DestroyWindow(app.hwnd);
        return;
    }
    if (action == "show") {
        show_main_window(app);
        ensure_master_password(app, app.hwnd);
        return;
    }
    if (action == "lock-all") {
        lock_all_dirs(app, L"一键全部锁定", true);
        return;
    }

    if (HWND active = win_get_active_modal_dialog()) {
        win_bring_window_to_front(active, true);
        return;
    }

    // External invocations from Explorer use an unowned topmost popup so background/minimized main window never locks or hides it.
    HWND ext_parent = nullptr;

    if (action == "unlock-file") {
        DirEntry* entry = nullptr;
        std::string file_id, orig_path;
        if (win_read_dlock_file(raw_path, file_id, orig_path)) {
            if (!file_id.empty()) entry = app.store->find_by_id(file_id);
            if (!entry && !orig_path.empty()) {
                entry = app.store->find_by_path(normalize_path_lexical(orig_path));
            }
        }
        if (!entry) {
            std::string stripped = raw_path;
            if (stripped.size() > 6 && _stricmp(stripped.c_str() + stripped.size() - 6, ".dlock") == 0) {
                stripped.resize(stripped.size() - 6);
                entry = app.store->find_by_path(normalize_path_lexical(stripped));
            }
        }
        if (!entry) {
            show_modal_msg(ext_parent, L"未找到对应的目录记录，请在「目录密匣」主界面中查看。",
                           L"提示", MB_ICONINFORMATION);
            return;
        }
        if (entry->locked) {
            unlock_path_flow(app, ext_parent, *entry, true);
        } else {
            open_in_explorer(entry->path);
        }
        return;
    }

    if (action == "unlock-id") {
        DirEntry* entry = app.store->find_by_id(raw_path);
        if (!entry) {
            show_modal_msg(ext_parent, L"未找到对应的目录记录，请在「目录密匣」主界面中查看。",
                           L"提示", MB_ICONINFORMATION);
            return;
        }
        if (entry->locked) {
            unlock_path_flow(app, ext_parent, *entry, true);
        } else {
            open_in_explorer(entry->path);
        }
        return;
    }
    if (raw_path.empty()) {
        show_main_window(app);
        return;
    }

    std::string canonical;
    std::string cerr;
    if (!canonicalize_existing_dir(raw_path, canonical, &cerr)) {
        show_modal_msg(ext_parent, utf8_to_wide(cerr), L"无法操作此目录", MB_ICONWARNING);
        return;
    }

    DirEntry* existing = app.store->find_by_path(canonical);

    if (action == "unlock") {
        if (!existing) {
            show_modal_msg(ext_parent, L"该目录尚未加入目录密匣保护。", L"提示", MB_ICONINFORMATION);
            return;
        }
        if (existing->locked) {
            unlock_path_flow(app, ext_parent, *existing, true);
        } else {
            open_in_explorer(existing->path);
        }
        return;
    }

    if (action == "lock") {
        if (existing) {
            if (existing->locked) {
                show_tray_balloon(app, L"目录密匣", L"该目录已处于锁定状态");
                return;
            }
            std::string lerr;
            if (!lock_existing_entry(app, *existing, false, &lerr)) {
                show_modal_msg(ext_parent, utf8_to_wide(lerr), L"锁定失败", MB_ICONERROR);
                return;
            }
            refresh_ui(app);
            show_tray_balloon(app, L"目录密匣", L"已重新锁定：\n" + utf8_to_wide(canonical));
            return;
        }
        lock_new_path_flow(app, ext_parent, canonical);
        return;
    }

    // "toggle"
    if (existing) {
        if (existing->locked) {
            unlock_path_flow(app, ext_parent, *existing, true);
        } else {
            std::string lerr;
            if (!lock_existing_entry(app, *existing, false, &lerr)) {
                show_modal_msg(ext_parent, utf8_to_wide(lerr), L"锁定失败", MB_ICONERROR);
                return;
            }
            refresh_ui(app);
            show_tray_balloon(app, L"目录密匣", L"已锁定目录：\n" + utf8_to_wide(canonical));
        }
    } else {
        lock_new_path_flow(app, ext_parent, canonical);
    }
}

int get_selected_row(WinApp& app) {
    int vis = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    if (vis < 0 || vis >= static_cast<int>(app.visible_entry_indices.size())) return -1;
    size_t idx = app.visible_entry_indices[static_cast<size_t>(vis)];
    if (idx >= app.store->entries().size()) return -1;
    return static_cast<int>(idx);
}

void do_add_and_lock(WinApp& app) {
    if (!ensure_master_password(app, app.hwnd)) return;
    std::string dir = win_pick_folder(app.hwnd);
    if (dir.empty()) return;
    lock_new_path_flow(app, app.hwnd, dir);
}

void do_unlock_selected(WinApp& app) {
    int row = get_selected_row(app);
    if (row < 0 || row >= static_cast<int>(app.store->entries().size())) {
        MessageBoxW(app.hwnd, L"请先在列表中选择一个目录。", L"提示", MB_ICONINFORMATION);
        return;
    }
    DirEntry& entry = app.store->entries_mut()[static_cast<size_t>(row)];
    if (!entry.locked) {
        open_in_explorer(entry.path);
        return;
    }
    unlock_path_flow(app, app.hwnd, entry);
}

void do_lock_selected(WinApp& app) {
    int row = get_selected_row(app);
    if (row < 0 || row >= static_cast<int>(app.store->entries().size())) {
        MessageBoxW(app.hwnd, L"请先在列表中选择一个目录。", L"提示", MB_ICONINFORMATION);
        return;
    }
    DirEntry& entry = app.store->entries_mut()[static_cast<size_t>(row)];
    if (entry.locked) {
        MessageBoxW(app.hwnd, L"该目录已经是锁定状态。", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::string err;
    if (!lock_existing_entry(app, entry, false, &err)) {
        MessageBoxW(app.hwnd, utf8_to_wide(err).c_str(), L"锁定失败", MB_ICONERROR);
        return;
    }
    refresh_ui(app);
}

bool apply_entry_custom_password(WinApp& app, DirEntry& entry,
                                 bool enable_custom, const std::string& new_pw) {
    if (enable_custom) {
        std::string h = hash_password(new_pw);
        if (h.empty()) {
            MessageBoxW(app.hwnd, L"生成密码哈希失败。", L"错误", MB_ICONERROR);
            return false;
        }
        entry.use_custom_password = true;
        entry.custom_hash = std::move(h);
    } else {
        entry.use_custom_password = false;
        entry.custom_hash.clear();
    }
    std::string err;
    if (!app.store->save(&err)) {
        MessageBoxW(app.hwnd, utf8_to_wide("保存状态失败: " + err).c_str(), L"错误", MB_ICONERROR);
        return false;
    }
    refresh_ui(app);
    return true;
}

void do_change_dir_password(WinApp& app) {
    int row = get_selected_row(app);
    if (row < 0 || row >= static_cast<int>(app.store->entries().size())) {
        MessageBoxW(app.hwnd, L"请先在列表中选择一个目录。", L"提示", MB_ICONINFORMATION);
        return;
    }
    DirEntry& entry = app.store->entries_mut()[static_cast<size_t>(row)];
    std::string cur_pw, new_pw;
    if (!win_show_change_dir_pw_dialog(app.hwnd, entry.path, entry.use_custom_password,
                                       cur_pw, new_pw)) {
        return;
    }
    if (!app.store->verify_entry_password(entry, cur_pw)) {
        MessageBoxW(app.hwnd, L"当前密码验证失败。", L"错误", MB_ICONWARNING);
        return;
    }
    bool target_custom = !entry.use_custom_password;
    if (!apply_entry_custom_password(app, entry, target_custom, new_pw)) return;
    MessageBoxW(app.hwnd,
                target_custom ? L"已切换为独立密码。" : L"已切换为主密码。",
                L"成功", MB_ICONINFORMATION);
}

void do_edit_custom_password(WinApp& app) {
    int row = get_selected_row(app);
    if (row < 0 || row >= static_cast<int>(app.store->entries().size())) {
        MessageBoxW(app.hwnd, L"请先在列表中选择一个目录。", L"提示", MB_ICONINFORMATION);
        return;
    }
    DirEntry& entry = app.store->entries_mut()[static_cast<size_t>(row)];
    if (!entry.use_custom_password) {
        MessageBoxW(
            app.hwnd,
            L"该目录当前使用的是主密码，未设置独立密码。\n\n"
            L"• 如需修改主密码，请点击右上角「设置 → 修改主密码」。\n"
            L"• 如需为该目录改用独立密码，请点击“切换密码方式”。",
            L"提示", MB_ICONINFORMATION);
        return;
    }
    std::string cur_pw, new_pw;
    if (!win_show_edit_custom_pw_dialog(app.hwnd, entry.path, cur_pw, new_pw)) {
        return;
    }
    if (!app.store->verify_entry_password(entry, cur_pw)) {
        MessageBoxW(app.hwnd, L"当前独立密码验证失败。", L"错误", MB_ICONWARNING);
        return;
    }
    if (!apply_entry_custom_password(app, entry, true, new_pw)) return;
    MessageBoxW(app.hwnd, L"该目录的独立密码已更新。", L"成功", MB_ICONINFORMATION);
}

void do_remove_selected(WinApp& app) {
    int row = get_selected_row(app);
    if (row < 0 || row >= static_cast<int>(app.store->entries().size())) {
        MessageBoxW(app.hwnd, L"请先选择一个目录。", L"提示", MB_ICONINFORMATION);
        return;
    }
    DirEntry& entry = app.store->entries_mut()[static_cast<size_t>(row)];
    std::string pw;
    bool dummy = false;
    if (!win_show_unlock_dir_dialog(app.hwnd, entry.path, entry.use_custom_password,
                                    false, L"验证密码以移出管理列表", L"", pw, dummy)) {
        return;
    }
    if (!app.store->verify_entry_password(entry, pw)) {
        MessageBoxW(app.hwnd, L"密码错误，无法移出受保护目录。", L"错误", MB_ICONWARNING);
        return;
    }
    if (entry.locked) {
        std::string err;
        bool has_target = !entry.vault_path.empty() ||
                          GetFileAttributesW(utf8_to_wide(entry.path).c_str()) != INVALID_FILE_ATTRIBUTES;
        if (has_target) {
            if (!win_smart_unlock(entry, &err)) {
                MessageBoxW(app.hwnd, utf8_to_wide("恢复目录失败，已取消移出: " + err).c_str(),
                            L"错误", MB_ICONERROR);
                return;
            }
        }
    }
    auto& vec = app.store->entries_mut();
    vec.erase(vec.begin() + row);
    std::string err;
    app.store->save(&err);
    refresh_ui(app);
}

void do_change_master_pw(WinApp& app) {
    HWND owner = (IsWindowVisible(app.hwnd) && !IsIconic(app.hwnd)) ? app.hwnd : nullptr;
    bool has_master = app.store->has_master_password();
    std::string old_pw, new_pw;
    if (!win_show_master_password_dialog(owner, has_master, old_pw, new_pw)) return;

    if (has_master && !app.store->verify_master_password(old_pw)) {
        show_modal_msg(owner, L"原主密码不正确。", L"错误", MB_ICONWARNING);
        return;
    }
    if (!app.store->set_master_password(new_pw)) {
        show_modal_msg(owner, L"生成密码哈希失败。", L"错误", MB_ICONERROR);
        return;
    }
    std::string err;
    app.store->save(&err);
    refresh_ui(app);
    show_modal_msg(owner, L"主密码已更新。", L"成功", MB_ICONINFORMATION);
}

void do_settings(WinApp& app) {
    HWND owner = (IsWindowVisible(app.hwnd) && !IsIconic(app.hwnd)) ? app.hwnd : nullptr;
    if (!ensure_master_password(app, owner)) return;

    int mins = app.store->settings().auto_relock_minutes;
    bool relock_on_lock = app.store->settings().relock_on_screen_lock;
    std::string pw;
    if (!win_show_settings_dialog(owner, mins, relock_on_lock, app.hotkey_ok, pw)) return;

    if (!app.store->verify_master_password(pw)) {
        show_modal_msg(owner, L"主密码错误，未保存设置。", L"验证失败", MB_ICONWARNING);
        return;
    }
    app.store->settings_mut().auto_relock_minutes = mins;
    app.store->settings_mut().relock_on_screen_lock = relock_on_lock;
    app.idle_triggered = false;
    std::string err;
    app.store->save(&err);
    refresh_ui(app);
    show_modal_msg(owner, L"安全设置已保存。", L"成功", MB_ICONINFORMATION);
}

void do_show_recovery(WinApp& app) {
    if (!ensure_master_password(app, app.hwnd)) return;
    std::string pw;
    if (!win_show_verify_master_dialog(
            app.hwnd, L"验证主密码",
            L"应急恢复说明包含手动恢复方法，为防止信息泄露，请先验证主密码：", pw)) {
        return;
    }
    if (!app.store->verify_master_password(pw)) {
        MessageBoxW(app.hwnd, L"主密码错误，无权查看应急恢复说明。", L"验证失败", MB_ICONWARNING);
        return;
    }
    win_show_recovery_dialog(app.hwnd);
}

void do_drop_files(WinApp& app, HDROP hDrop) {
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::string> dirs;
    std::vector<std::string> non_dirs;

    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring wp(len + 1, L'\0');
        DragQueryFileW(hDrop, i, &wp[0], len + 1);
        wp.resize(len);

        DWORD attr = GetFileAttributesW(wp.c_str());
        std::string utf8 = wide_to_utf8(wp);
        if (attr == INVALID_FILE_ATTRIBUTES ||
            (attr & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            non_dirs.push_back(utf8);
        } else {
            dirs.push_back(utf8);
        }
    }
    DragFinish(hDrop);

    if (!non_dirs.empty()) {
        std::string msg = "拖入内容中包含非目录项目（请仅拖入真实目录；如需解锁已加密目录请直接双击对应带锁图标）：\n";
        for (size_t i = 0; i < non_dirs.size() && i < 6; ++i) {
            msg += "• " + non_dirs[i] + "\n";
        }
        if (non_dirs.size() > 6) {
            msg += "等共 " + std::to_string(non_dirs.size()) + " 项";
        }
        MessageBoxW(app.hwnd, utf8_to_wide(msg).c_str(), L"拖入错误", MB_ICONWARNING);
        return;
    }

    if (!dirs.empty()) {
        lock_new_paths_batch(app, app.hwnd, dirs);
    }
}

void do_quit(WinApp& app) {
    int unlocked = 0;
    for (const auto& e : app.store->entries()) {
        if (!e.locked) ++unlocked;
    }
    if (unlocked > 0) {
        std::wstring msg = L"当前仍有 " + std::to_wstring(unlocked) +
                           L" 个受管目录处于解锁状态，是否全部锁定后再退出？";
        int ans = MessageBoxW(app.hwnd, msg.c_str(), L"退出确认", MB_ICONQUESTION | MB_YESNOCANCEL);
        if (ans == IDCANCEL) return;
        if (ans == IDYES) {
            lock_all_dirs(app, L"退出前全锁", false);
        }
    }
    app.really_quit = true;
    DestroyWindow(app.hwnd);
}

constexpr int kWinCornerRadius = 6;

Gdiplus::Color gp_color(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void build_round_rect_path(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y,
                           Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r) {
    Gdiplus::REAL max_r = (w < h ? w : h) * 0.5f;
    if (r > max_r) r = max_r;
    if (r < 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    Gdiplus::REAL d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void gp_draw_round_rect(Gdiplus::Graphics& g, RECT rc, int radius_px,
                        COLORREF bg, COLORREF border, int dpi, bool draw_border = true) {
    Gdiplus::REAL w = static_cast<Gdiplus::REAL>(rc.right - rc.left) - 1.0f;
    Gdiplus::REAL h = static_cast<Gdiplus::REAL>(rc.bottom - rc.top) - 1.0f;
    if (w < 1.0f || h < 1.0f) return;
    Gdiplus::REAL x = static_cast<Gdiplus::REAL>(rc.left) + 0.5f;
    Gdiplus::REAL y = static_cast<Gdiplus::REAL>(rc.top) + 0.5f;
    Gdiplus::REAL r = static_cast<Gdiplus::REAL>(scale_dpi(radius_px, dpi));

    Gdiplus::GraphicsPath path;
    build_round_rect_path(path, x, y, w, h, r);
    Gdiplus::SolidBrush fill(gp_color(bg));
    g.FillPath(&fill, &path);
    if (draw_border) {
        Gdiplus::Pen pen(gp_color(border), 1.0f);
        g.DrawPath(&pen, &path);
    }
}

struct WinCellLayout {
    RECT status_chip{};
    RECT tag_rect{};
    RECT action_btn{};
    RECT open_btn{};
    RECT more_btn{};
};

WinCellLayout compute_win_cell_layout(RECT cell, int col, bool locked, int dpi) {
    auto S = [&](int px) { return scale_dpi(px, dpi); };
    WinCellLayout lay{};
    int cy = cell.top + (cell.bottom - cell.top) / 2;
    if (col == 1) {
        int sz = S(28);
        int l = cell.left + ((cell.right - cell.left) - sz) / 2;
        int t = cy - sz / 2;
        lay.status_chip = RECT{l, t, l + sz, t + sz};
    } else if (col == 2) {
        int w = S(88);
        int max_w = (cell.right - cell.left) - S(10);
        if (w > max_w) w = max_w;
        int h = S(28);
        int l = cell.left + ((cell.right - cell.left) - w) / 2;
        int t = cy - h / 2;
        lay.tag_rect = RECT{l, t, l + w, t + h};
    } else if (col == 3) {
        int action_w = S(64);
        int icon_w = S(28);
        int gap = S(6);
        int total_w = icon_w + gap + action_w + gap + icon_w;  // S(132)
        int h = S(28);
        int start_x = cell.left + ((cell.right - cell.left) - total_w) / 2;
        if (start_x < cell.left + S(6)) start_x = cell.left + S(6);
        int t = cy - h / 2;
        if (!locked) {
            lay.open_btn = RECT{start_x, t, start_x + icon_w, t + h};
        }
        int ax = start_x + icon_w + gap;
        lay.action_btn = RECT{ax, t, ax + action_w, t + h};
        int mx = ax + action_w + gap;
        lay.more_btn = RECT{mx, t, mx + icon_w, t + h};
    }
    return lay;
}

int hit_win_cell_slot(RECT cell, int col, bool locked, POINT pt, int dpi) {
    WinCellLayout lay = compute_win_cell_layout(cell, col, locked, dpi);
    if (col == 1 && PtInRect(&lay.status_chip, pt)) return 1;
    if (col == 2 && PtInRect(&lay.tag_rect, pt)) return 1;
    if (col == 3) {
        if (PtInRect(&lay.action_btn, pt)) return 1;
        if (!locked && PtInRect(&lay.open_btn, pt)) return 2;
        if (PtInRect(&lay.more_btn, pt)) return 3;
    }
    return 0;
}

void draw_status_lock_chip(Gdiplus::Graphics& g, RECT rc, bool locked, int dpi) {
    COLORREF bg = locked ? RGB(227, 242, 253) : RGB(232, 245, 233);
    COLORREF border = locked ? RGB(144, 202, 249) : RGB(165, 214, 167);
    COLORREF fg = locked ? RGB(13, 71, 161) : RGB(27, 94, 32);
    gp_draw_round_rect(g, rc, kWinCornerRadius, bg, border, dpi);

    Gdiplus::REAL s = static_cast<Gdiplus::REAL>(dpi) / 96.0f;
    Gdiplus::REAL cx = (rc.left + rc.right) * 0.5f;
    Gdiplus::REAL cy = (rc.top + rc.bottom) * 0.5f;

    Gdiplus::Pen shackle_pen(gp_color(fg), 1.8f * s);
    shackle_pen.SetStartCap(Gdiplus::LineCapRound);
    shackle_pen.SetEndCap(Gdiplus::LineCapRound);
    shackle_pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::GraphicsPath shackle;
    if (locked) {
        shackle.AddLine(cx - 3.6f * s, cy - 1.0f * s, cx - 3.6f * s, cy - 4.0f * s);
        shackle.AddArc(cx - 3.6f * s, cy - 7.6f * s, 7.2f * s, 7.2f * s, 180.0f, 180.0f);
        shackle.AddLine(cx + 3.6f * s, cy - 4.0f * s, cx + 3.6f * s, cy - 1.0f * s);
    } else {
        shackle.AddLine(cx - 3.6f * s, cy - 1.0f * s, cx - 3.6f * s, cy - 4.6f * s);
        shackle.AddArc(cx - 3.6f * s, cy - 8.2f * s, 7.2f * s, 7.2f * s, 180.0f, 160.0f);
    }
    g.DrawPath(&shackle_pen, &shackle);

    Gdiplus::GraphicsPath body;
    build_round_rect_path(body, cx - 5.4f * s, cy - 1.2f * s, 10.8f * s, 8.2f * s, 2.2f * s);
    Gdiplus::SolidBrush body_br(gp_color(fg));
    g.FillPath(&body_br, &body);

    Gdiplus::SolidBrush hole_br(gp_color(bg));
    Gdiplus::REAL r = 1.35f * s;
    g.FillEllipse(&hole_br, cx - r, cy + 2.9f * s - r, r * 2.0f, r * 2.0f);
}

void draw_folder_action_icon(Gdiplus::Graphics& g, RECT rc, bool hovered, int dpi) {
    COLORREF bg = hovered ? RGB(255, 236, 179) : RGB(255, 248, 225);       // #FFF8E1 / #FFECB3 light yellow chip
    COLORREF border = hovered ? RGB(217, 119, 6) : RGB(246, 196, 83);      // #F6C453 / #D97706
    COLORREF inner_fill = hovered ? RGB(252, 211, 77) : RGB(253, 230, 138); // #FDE68A warm yellow fill
    COLORREF fg = RGB(180, 83, 9);                                         // #B45309 deep amber bold stroke
    gp_draw_round_rect(g, rc, kWinCornerRadius, bg, border, dpi);

    Gdiplus::REAL s = static_cast<Gdiplus::REAL>(dpi) / 96.0f;
    Gdiplus::REAL cx = (rc.left + rc.right) * 0.5f;
    Gdiplus::REAL cy = (rc.top + rc.bottom) * 0.5f;
    Gdiplus::PointF pts[6] = {
        {cx - 6.2f * s, cy - 4.4f * s},
        {cx - 1.8f * s, cy - 4.4f * s},
        {cx - 0.3f * s, cy - 2.4f * s},
        {cx + 6.2f * s, cy - 2.4f * s},
        {cx + 6.2f * s, cy + 4.6f * s},
        {cx - 6.2f * s, cy + 4.6f * s}
    };
    Gdiplus::GraphicsPath p;
    p.AddPolygon(pts, 6);
    Gdiplus::SolidBrush fbr(gp_color(inner_fill));
    Gdiplus::Pen fpen(gp_color(fg), 1.9f * s);
    fpen.SetStartCap(Gdiplus::LineCapRound);
    fpen.SetEndCap(Gdiplus::LineCapRound);
    fpen.SetLineJoin(Gdiplus::LineJoinRound);
    g.FillPath(&fbr, &p);
    g.DrawPath(&fpen, &p);
    g.DrawLine(&fpen, cx - 6.0f * s, cy - 1.3f * s, cx + 6.0f * s, cy - 1.3f * s);
}

void draw_more_dots_icon(Gdiplus::Graphics& g, RECT rc, bool hovered, int dpi) {
    COLORREF bg = hovered ? RGB(243, 244, 246) : RGB(255, 255, 255);
    COLORREF border = hovered ? RGB(175, 184, 193) : RGB(208, 215, 222);
    COLORREF fg = hovered ? RGB(31, 35, 40) : RGB(95, 99, 104);
    gp_draw_round_rect(g, rc, kWinCornerRadius, bg, border, dpi);

    Gdiplus::REAL s = static_cast<Gdiplus::REAL>(dpi) / 96.0f;
    Gdiplus::REAL cx = (rc.left + rc.right) * 0.5f;
    Gdiplus::REAL cy = (rc.top + rc.bottom) * 0.5f;
    Gdiplus::REAL r = 1.4f * s;
    Gdiplus::SolidBrush br(gp_color(fg));
    g.FillEllipse(&br, cx - 4.5f * s - r, cy - r, r * 2.0f, r * 2.0f);
    g.FillEllipse(&br, cx - r, cy - r, r * 2.0f, r * 2.0f);
    g.FillEllipse(&br, cx + 4.5f * s - r, cy - r, r * 2.0f, r * 2.0f);
}

void draw_dir_list_item(WinApp& app, const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == static_cast<UINT>(-1)) return;
    size_t vis = static_cast<size_t>(dis->itemID);
    if (vis >= app.visible_entry_indices.size()) return;
    size_t entry_idx = app.visible_entry_indices[vis];
    if (entry_idx >= app.store->entries().size()) return;

    const DirEntry& e = app.store->entries()[entry_idx];
    const RECT rc = dis->rcItem;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    auto S = [&](int px) { return scale_dpi(px, app.dpi); };

    HDC mem = CreateCompatibleDC(dis->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(dis->hDC, w, h);
    HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    RECT lrc{0, 0, w, h};
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const int vis_i = static_cast<int>(vis);
    const bool row_hovered = (app.hover_row == vis_i);
    COLORREF row_bg = selected ? RGB(235, 243, 254) : (row_hovered ? RGB(246, 248, 250) : RGB(255, 255, 255));
    HBRUSH row_br = CreateSolidBrush(row_bg);
    FillRect(mem, &lrc, row_br);
    DeleteObject(row_br);

    // Subtle bottom hairline
    HPEN sep_pen = CreatePen(PS_SOLID, 1, RGB(238, 241, 244));
    HPEN old_pen = static_cast<HPEN>(SelectObject(mem, sep_pen));
    MoveToEx(mem, 0, h - 1, nullptr);
    LineTo(mem, w, h - 1);
    SelectObject(mem, old_pen);
    DeleteObject(sep_pen);

    if (selected) {
        RECT bar{0, S(4), S(3), h - S(4)};
        HBRUSH bar_br = CreateSolidBrush(RGB(13, 71, 161));
        FillRect(mem, &bar, bar_br);
        DeleteObject(bar_br);
    }

    int c0_w = ListView_GetColumnWidth(app.list, 0);
    int c1_w = ListView_GetColumnWidth(app.list, 1);
    int c2_w = ListView_GetColumnWidth(app.list, 2);
    int c3_w = ListView_GetColumnWidth(app.list, 3);

    // Column 0: Icon + Bold Folder Name + Muted Full Path
    int icon_sz = S(28);
    int icon_x = S(14);
    int icon_y = (h - icon_sz) / 2;
    HICON ic = e.locked ? app.folder_lock_icon : app.folder_plain_icon;
    if (ic) {
        DrawIconEx(mem, icon_x, icon_y, ic, icon_sz, icon_sz, 0, nullptr, DI_NORMAL);
    }

    int text_x = icon_x + icon_sz + S(12);
    int text_r = c0_w - S(10);
    if (text_r > text_x + S(20)) {
        SetBkMode(mem, TRANSPARENT);
        std::wstring wname = utf8_to_wide(basename_utf8(e.path));
        std::wstring wpath = utf8_to_wide(e.path);

        RECT nr{text_x, S(7), text_r, S(27)};
        HFONT old_f = static_cast<HFONT>(SelectObject(mem, app.bold_font ? app.bold_font : app.font));
        SetTextColor(mem, RGB(31, 35, 40));
        DrawTextW(mem, wname.c_str(), static_cast<int>(wname.size()), &nr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        RECT pr{text_x, S(27), text_r, h - S(7)};
        SelectObject(mem, app.small_font ? app.small_font : app.font);
        SetTextColor(mem, RGB(95, 99, 104));
        DrawTextW(mem, wpath.c_str(), static_cast<int>(wpath.size()), &pr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        SelectObject(mem, old_f);
    }

    RECT c1{c0_w, 0, c0_w + c1_w, h};
    RECT c2{c0_w + c1_w, 0, c0_w + c1_w + c2_w, h};
    RECT c3{c0_w + c1_w + c2_w, 0, c0_w + c1_w + c2_w + c3_w, h};
    WinCellLayout l1 = compute_win_cell_layout(c1, 1, e.locked, app.dpi);
    WinCellLayout l2 = compute_win_cell_layout(c2, 2, e.locked, app.dpi);
    WinCellLayout l3 = compute_win_cell_layout(c3, 3, e.locked, app.dpi);

    const bool hov_tag = (row_hovered && app.hover_col == 2 && app.hover_slot == 1);
    const bool hov_op1 = (row_hovered && app.hover_col == 3 && app.hover_slot == 1);
    const bool hov_op2 = (row_hovered && app.hover_col == 3 && app.hover_slot == 2);
    const bool hov_op3 = (row_hovered && app.hover_col == 3 && app.hover_slot == 3);

    COLORREF dot_c = e.use_custom_password ? RGB(217, 119, 6) : RGB(100, 116, 139);
    COLORREF tag_fg = e.use_custom_password ? RGB(31, 35, 40) : RGB(71, 85, 105);

    COLORREF act_bg, act_bd, act_fg;
    if (e.locked) {
        act_bg = hov_op1 ? RGB(220, 242, 223) : RGB(232, 245, 233);
        act_bd = hov_op1 ? RGB(46, 125, 50) : RGB(129, 199, 132);
        act_fg = RGB(27, 94, 32);
    } else {
        act_bg = hov_op1 ? RGB(220, 235, 250) : RGB(235, 243, 254);
        act_bd = hov_op1 ? RGB(25, 118, 210) : RGB(144, 202, 249);
        act_fg = RGB(13, 71, 161);
    }

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // Col 1: Status padlock chip
        draw_status_lock_chip(g, l1.status_chip, e.locked, app.dpi);

        // Col 2: Borderless dot + text by default; subtle 6px hover frame on mouseover
        if (hov_tag) {
            COLORREF hov_bg = selected ? RGB(255, 255, 255) : RGB(241, 245, 249);
            COLORREF hov_bd = selected ? RGB(144, 202, 249) : RGB(203, 213, 225);
            gp_draw_round_rect(g, l2.tag_rect, kWinCornerRadius, hov_bg, hov_bd, app.dpi);
        }
        Gdiplus::REAL s = static_cast<Gdiplus::REAL>(app.dpi) / 96.0f;
        Gdiplus::REAL dot_cx = static_cast<Gdiplus::REAL>(l2.tag_rect.left) + 16.0f * s;
        Gdiplus::REAL dot_cy = (l2.tag_rect.top + l2.tag_rect.bottom) * 0.5f;
        Gdiplus::REAL dot_r = 3.4f * s;
        Gdiplus::SolidBrush dot_br(gp_color(dot_c));
        g.FillEllipse(&dot_br, dot_cx - dot_r, dot_cy - dot_r, dot_r * 2.0f, dot_r * 2.0f);

        // Col 3: Primary row action + optional folder icon + more menu dots (all 6px radius)
        gp_draw_round_rect(g, l3.action_btn, kWinCornerRadius, act_bg, act_bd, app.dpi);
        if (!e.locked) {
            draw_folder_action_icon(g, l3.open_btn, hov_op2, app.dpi);
        }
        draw_more_dots_icon(g, l3.more_btn, hov_op3, app.dpi);
    }

    SetBkMode(mem, TRANSPARENT);
    HFONT old_font = static_cast<HFONT>(SelectObject(mem, app.font));
    SetTextColor(mem, tag_fg);
    std::wstring mode_txt = e.use_custom_password ? L"独立密码" : L"主密码";
    RECT tag_text_rc{l2.tag_rect.left + S(27), l2.tag_rect.top, l2.tag_rect.right - S(4), l2.tag_rect.bottom};
    DrawTextW(mem, mode_txt.c_str(), static_cast<int>(mode_txt.size()), &tag_text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem, app.bold_font ? app.bold_font : app.font);
    SetTextColor(mem, act_fg);
    std::wstring act_txt = e.locked ? L"解锁" : L"锁定";
    DrawTextW(mem, act_txt.c_str(), static_cast<int>(act_txt.size()), &l3.action_btn,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mem, old_font);

    BitBlt(dis->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void draw_modern_button(WinApp& app, const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    auto S = [&](int px) { return scale_dpi(px, app.dpi); };
    const RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool hovered = (app.hovered_btn == dis->hwndItem);
    const int id = static_cast<int>(dis->CtlID);

    const bool is_primary = (id == IDC_BTN_ADD);
    const bool is_tonal_accent = (id == IDC_BTN_LOCK_ALL);

    if (app.bg_brush) FillRect(dis->hDC, &rc, app.bg_brush);

    COLORREF bg, border, fg;
    if (disabled) {
        bg = RGB(246, 248, 250);
        border = RGB(225, 228, 232);
        fg = RGB(154, 160, 166);
    } else if (is_primary) {
        bg = pressed ? RGB(9, 53, 122) : (hovered ? RGB(21, 101, 192) : RGB(13, 71, 161));
        border = bg;
        fg = RGB(255, 255, 255);
    } else if (is_tonal_accent) {
        bg = pressed ? RGB(203, 226, 251) : (hovered ? RGB(220, 235, 250) : RGB(235, 243, 254));
        border = pressed ? RGB(13, 71, 161) : (hovered ? RGB(25, 118, 210) : RGB(144, 202, 249));
        fg = RGB(13, 71, 161);
    } else {
        bg = pressed ? RGB(235, 236, 240) : (hovered ? RGB(243, 244, 246) : RGB(255, 255, 255));
        border = pressed ? RGB(175, 184, 193) : (hovered ? RGB(175, 184, 193) : RGB(208, 215, 222));
        fg = RGB(31, 35, 40);
    }

    {
        Gdiplus::Graphics g(dis->hDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        gp_draw_round_rect(g, rc, kWinCornerRadius, bg, border, app.dpi);
    }

    std::wstring text = get_window_text_w_main(dis->hwndItem);
    HFONT use_font = (is_primary || is_tonal_accent) && app.bold_font ? app.bold_font : app.font;
    HFONT old_font = static_cast<HFONT>(SelectObject(dis->hDC, use_font));
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    RECT tr = rc;
    DrawTextW(dis->hDC, text.c_str(), static_cast<int>(text.size()), &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old_font);

    if ((dis->itemState & ODS_FOCUS) && !disabled && !is_primary) {
        RECT fr = rc;
        InflateRect(&fr, -S(3), -S(3));
        DrawFocusRect(dis->hDC, &fr);
    }
}

enum class WinMenuIcon {
    Unlock,
    OpenFolder,
    Lock,
    Key,
    Switch,
    Trash
};

HBITMAP create_win_menu_icon(WinMenuIcon kind, int px) {
    if (px < 12) px = 16;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = px;
    bmi.bmiHeader.biHeight = -px;  // top-down 32bpp PARGB DIB required by Windows themed menus
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) return nullptr;
    std::memset(bits, 0, static_cast<size_t>(px) * static_cast<size_t>(px) * 4);

    {
        Gdiplus::Bitmap bm(px, px, px * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bm);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const Gdiplus::REAL s = static_cast<Gdiplus::REAL>(px) / 16.0f;
        Gdiplus::Pen pen(Gdiplus::Color(255, 51, 65, 85), 1.4f * s);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);

        switch (kind) {
            case WinMenuIcon::Unlock: {
                Gdiplus::GraphicsPath sh;
                sh.AddLine(5.2f * s, 7.0f * s, 5.2f * s, 4.8f * s);
                sh.AddArc(5.2f * s, 2.2f * s, 5.6f * s, 5.0f * s, 180.0f, 150.0f);
                g.DrawPath(&pen, &sh);
                Gdiplus::GraphicsPath body;
                build_round_rect_path(body, 3.3f * s, 7.0f * s, 9.4f * s, 6.6f * s, 1.6f * s);
                g.DrawPath(&pen, &body);
                g.DrawLine(&pen, 8.0f * s, 9.6f * s, 8.0f * s, 11.4f * s);
                break;
            }
            case WinMenuIcon::OpenFolder: {
                Gdiplus::PointF pts[6] = {
                    {2.0f * s, 3.6f * s}, {6.4f * s, 3.6f * s}, {7.8f * s, 5.1f * s},
                    {14.0f * s, 5.1f * s}, {14.0f * s, 12.6f * s}, {2.0f * s, 12.6f * s}
                };
                Gdiplus::GraphicsPath fp;
                fp.AddPolygon(pts, 6);
                g.DrawPath(&pen, &fp);
                g.DrawLine(&pen, 2.0f * s, 6.6f * s, 14.0f * s, 6.6f * s);
                break;
            }
            case WinMenuIcon::Lock: {
                Gdiplus::GraphicsPath sh;
                sh.AddLine(5.2f * s, 7.0f * s, 5.2f * s, 4.8f * s);
                sh.AddArc(5.2f * s, 2.2f * s, 5.6f * s, 5.0f * s, 180.0f, 180.0f);
                sh.AddLine(10.8f * s, 4.8f * s, 10.8f * s, 7.0f * s);
                g.DrawPath(&pen, &sh);
                Gdiplus::GraphicsPath body;
                build_round_rect_path(body, 3.3f * s, 7.0f * s, 9.4f * s, 6.6f * s, 1.6f * s);
                g.DrawPath(&pen, &body);
                g.DrawLine(&pen, 8.0f * s, 9.6f * s, 8.0f * s, 11.4f * s);
                break;
            }
            case WinMenuIcon::Key: {
                g.DrawEllipse(&pen, 2.8f * s, 8.2f * s, 4.8f * s, 4.8f * s);
                g.DrawLine(&pen, 7.0f * s, 8.8f * s, 13.0f * s, 2.8f * s);
                g.DrawLine(&pen, 10.7f * s, 5.1f * s, 12.3f * s, 6.7f * s);
                g.DrawLine(&pen, 12.4f * s, 3.4f * s, 13.7f * s, 4.7f * s);
                break;
            }
            case WinMenuIcon::Switch: {
                g.DrawLine(&pen, 2.6f * s, 5.2f * s, 13.2f * s, 5.2f * s);
                g.DrawLine(&pen, 10.7f * s, 3.0f * s, 13.2f * s, 5.2f * s);
                g.DrawLine(&pen, 10.7f * s, 7.4f * s, 13.2f * s, 5.2f * s);
                g.DrawLine(&pen, 13.4f * s, 10.8f * s, 2.8f * s, 10.8f * s);
                g.DrawLine(&pen, 5.3f * s, 8.6f * s, 2.8f * s, 10.8f * s);
                g.DrawLine(&pen, 5.3f * s, 13.0f * s, 2.8f * s, 10.8f * s);
                break;
            }
            case WinMenuIcon::Trash: {
                g.DrawLine(&pen, 6.0f * s, 4.2f * s, 6.0f * s, 2.7f * s);
                g.DrawLine(&pen, 6.0f * s, 2.7f * s, 10.0f * s, 2.7f * s);
                g.DrawLine(&pen, 10.0f * s, 2.7f * s, 10.0f * s, 4.2f * s);
                g.DrawLine(&pen, 2.6f * s, 4.4f * s, 13.4f * s, 4.4f * s);
                Gdiplus::PointF bin[4] = {
                    {3.9f * s, 4.4f * s}, {4.6f * s, 13.2f * s},
                    {11.4f * s, 13.2f * s}, {12.1f * s, 4.4f * s}
                };
                Gdiplus::GraphicsPath bp;
                bp.AddLines(bin, 4);
                g.DrawPath(&pen, &bp);
                g.DrawLine(&pen, 6.7f * s, 6.7f * s, 6.9f * s, 11.0f * s);
                g.DrawLine(&pen, 9.3f * s, 6.7f * s, 9.1f * s, 11.0f * s);
                break;
            }
        }
    }
    return hbmp;
}

void show_row_context_menu(WinApp& app, int vis_row, POINT screen_pt, UINT align_flags, bool more_menu_only) {
    if (vis_row < 0 || vis_row >= static_cast<int>(app.visible_entry_indices.size())) return;
    size_t idx = app.visible_entry_indices[static_cast<size_t>(vis_row)];
    if (idx >= app.store->entries().size()) return;
    const auto& entry = app.store->entries()[idx];

    HMENU menu = CreatePopupMenu();
    std::vector<HBITMAP> menu_bmps;
    const int icon_px = scale_dpi(16, app.dpi);
    auto append_item = [&](UINT id, const wchar_t* text, WinMenuIcon kind) {
        AppendMenuW(menu, MF_STRING, id, text);
        if (HBITMAP bmp = create_win_menu_icon(kind, icon_px)) {
            MENUITEMINFOW mii{};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_BITMAP;
            mii.hbmpItem = bmp;
            SetMenuItemInfoW(menu, id, FALSE, &mii);
            menu_bmps.push_back(bmp);
        }
    };

    if (!more_menu_only) {
        if (entry.locked) {
            append_item(IDM_ROW_UNLOCK_OR_OPEN, L"解锁并打开", WinMenuIcon::Unlock);
        } else {
            append_item(IDM_ROW_UNLOCK_OR_OPEN, L"打开目录", WinMenuIcon::OpenFolder);
            append_item(IDM_ROW_LOCK, L"重新锁定", WinMenuIcon::Lock);
        }
    }
    if (entry.use_custom_password) {
        append_item(IDM_ROW_EDIT_CUSTOM_PW, L"修改独立密码", WinMenuIcon::Key);
        append_item(IDM_ROW_DIR_PW, L"切换为主密码", WinMenuIcon::Switch);
    } else {
        append_item(IDM_ROW_DIR_PW, L"切换为独立密码", WinMenuIcon::Switch);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    append_item(IDM_ROW_REMOVE, L"移出保护列表", WinMenuIcon::Trash);
    SetForegroundWindow(app.hwnd);
    TrackPopupMenu(menu, align_flags, screen_pt.x, screen_pt.y, 0, app.hwnd, nullptr);
    PostMessageW(app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    for (HBITMAP hb : menu_bmps) {
        if (hb) DeleteObject(hb);
    }
}

LRESULT CALLBACK HoverBtnSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"OrigBtnProc"));
    auto* app = reinterpret_cast<WinApp*>(GetPropW(hwnd, L"DirLockerApp"));
    if (msg == WM_MOUSEMOVE && app) {
        if (app->hovered_btn != hwnd) {
            HWND old = app->hovered_btn;
            app->hovered_btn = hwnd;
            if (old) InvalidateRect(old, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
    } else if (msg == WM_MOUSELEAVE && app) {
        if (app->hovered_btn == hwnd) {
            app->hovered_btn = nullptr;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"OrigBtnProc");
        RemovePropW(hwnd, L"DirLockerApp");
    }
    return orig ? CallWindowProcW(orig, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<WinApp*>(GetPropW(hwnd, L"DirLockerApp"));
    if (app && msg == WM_MOUSEMOVE) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        LVHITTESTINFO hti{};
        hti.pt = pt;
        ListView_SubItemHitTest(hwnd, &hti);
        int new_row = hti.iItem;
        int new_col = hti.iSubItem;
        int new_slot = 0;
        std::wstring new_tip;

        if (new_row >= 0 && new_row < static_cast<int>(app->visible_entry_indices.size())) {
            const DirEntry& e = app->store->entries()[app->visible_entry_indices[static_cast<size_t>(new_row)]];
            if (new_col >= 1 && new_col <= 3) {
                RECT sub_rc{};
                ListView_GetSubItemRect(hwnd, new_row, new_col, LVIR_BOUNDS, &sub_rc);
                new_slot = hit_win_cell_slot(sub_rc, new_col, e.locked, pt, app->dpi);
            }
            if (new_col == 1 && new_slot == 1) {
                new_tip = e.locked ? L"当前状态：已锁定（双击可解锁）" : L"当前状态：已解锁（双击可锁定）";
            } else if (new_col == 2 && new_slot == 1) {
                new_tip = e.use_custom_password ? L"双击可切换为主密码"
                                                : L"双击可切换为独立密码";
            } else if (new_col == 3) {
                if (new_slot == 1) new_tip = e.locked ? L"输入密码解锁并打开" : L"立即重新锁定此目录";
                else if (new_slot == 2) new_tip = L"打开目录";
                else if (new_slot == 3) new_tip = L"更多操作（改密、切换方式、移出列表）";
            }
        }

        if (new_row != app->hover_row || new_col != app->hover_col || new_slot != app->hover_slot) {
            int old_row = app->hover_row;
            app->hover_row = new_row;
            app->hover_col = new_col;
            app->hover_slot = new_slot;

            if (old_row >= 0) {
                RECT r{};
                if (ListView_GetItemRect(hwnd, old_row, &r, LVIR_BOUNDS)) InvalidateRect(hwnd, &r, FALSE);
            }
            if (new_row >= 0) {
                RECT r{};
                if (ListView_GetItemRect(hwnd, new_row, &r, LVIR_BOUNDS)) InvalidateRect(hwnd, &r, FALSE);
            }

            if (app->list_tip && new_tip != app->tip_text) {
                app->tip_text = new_tip;
                SendMessageW(app->list_tip, TTM_POP, 0, 0);
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.hwnd = app->hwnd;
                ti.uId = reinterpret_cast<UINT_PTR>(hwnd);
                ti.lpszText = const_cast<LPWSTR>(app->tip_text.c_str());
                SendMessageW(app->list_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
            }
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    } else if (app && msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        if (app->hover_col == 3 && app->hover_slot > 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
    } else if (app && msg == WM_MOUSELEAVE) {
        int old_row = app->hover_row;
        app->hover_row = -1;
        app->hover_col = -1;
        app->hover_slot = 0;
        if (old_row >= 0) {
            RECT r{};
            if (ListView_GetItemRect(hwnd, old_row, &r, LVIR_BOUNDS)) InvalidateRect(hwnd, &r, FALSE);
        }
        if (app->list_tip) {
            app->tip_text.clear();
            SendMessageW(app->list_tip, TTM_POP, 0, 0);
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.hwnd = app->hwnd;
            ti.uId = reinterpret_cast<UINT_PTR>(hwnd);
            ti.lpszText = const_cast<LPWSTR>(L"");
            SendMessageW(app->list_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"DirLockerApp");
    }
    return (app && app->orig_list_proc) ? CallWindowProcW(app->orig_list_proc, hwnd, msg, wp, lp)
                                        : DefWindowProcW(hwnd, msg, wp, lp);
}

void show_button_popup_menu(HWND owner, HWND btn, const std::vector<std::pair<UINT, std::wstring>>& items) {
    RECT rc{};
    GetWindowRect(btn, &rc);
    HMENU menu = CreatePopupMenu();
    for (const auto& it : items) {
        if (it.first == 0) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else {
            AppendMenuW(menu, MF_STRING, it.first, it.second.c_str());
        }
    }
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, rc.left, rc.bottom + 2, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK EmptyOverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        auto* app = reinterpret_cast<WinApp*>(GetPropW(hwnd, L"DirLockerApp"));
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr{};
        GetClientRect(hwnd, &cr);
        int w = cr.right - cr.left;
        int h = cr.bottom - cr.top;
        if (app && w > 0 && h > 0) {
            auto S = [&](int px) { return scale_dpi(px, app->dpi); };
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
            HBRUSH bg_br = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(mem, &cr, bg_br);
            DeleteObject(bg_br);

            int bw = w - S(48);
            if (bw > S(440)) bw = S(440);
            int bh = h - S(32);
            if (bh > S(186)) bh = S(186);

            if (bw >= S(220) && bh >= S(120)) {
                int bx = (w - bw) / 2;
                int by = (h - bh) / 2;
                const bool filtered_empty = app->store && !app->store->entries().empty();

                {
                    Gdiplus::Graphics g(mem);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    Gdiplus::REAL s = static_cast<Gdiplus::REAL>(app->dpi) / 96.0f;

                    Gdiplus::GraphicsPath box;
                    build_round_rect_path(box, static_cast<Gdiplus::REAL>(bx),
                                          static_cast<Gdiplus::REAL>(by),
                                          static_cast<Gdiplus::REAL>(bw),
                                          static_cast<Gdiplus::REAL>(bh), 10.0f * s);
                    Gdiplus::SolidBrush fill_br(Gdiplus::Color(255, 248, 250, 252));
                    g.FillPath(&fill_br, &box);
                    Gdiplus::Pen dash_pen(Gdiplus::Color(255, 203, 213, 225), 1.2f * s);
                    dash_pen.SetDashStyle(Gdiplus::DashStyleDash);
                    g.DrawPath(&dash_pen, &box);

                    Gdiplus::REAL cx = bx + bw * 0.5f;
                    Gdiplus::REAL cy = by + 50.0f * s;
                    Gdiplus::REAL cr_rad = 24.0f * s;
                    Gdiplus::SolidBrush badge_br(Gdiplus::Color(255, 235, 243, 254));
                    g.FillEllipse(&badge_br, cx - cr_rad, cy - cr_rad, cr_rad * 2.0f, cr_rad * 2.0f);

                    Gdiplus::PointF fpts[6] = {
                        {cx - 12.0f * s, cy - 8.0f * s}, {cx - 4.0f * s, cy - 8.0f * s},
                        {cx - 1.0f * s, cy - 4.0f * s},  {cx + 12.0f * s, cy - 4.0f * s},
                        {cx + 12.0f * s, cy + 9.0f * s}, {cx - 12.0f * s, cy + 9.0f * s}
                    };
                    Gdiplus::GraphicsPath fp;
                    fp.AddPolygon(fpts, 6);
                    Gdiplus::SolidBrush ffill(Gdiplus::Color(255, 219, 234, 254));
                    Gdiplus::Pen fpen(Gdiplus::Color(255, 13, 71, 161), 1.8f * s);
                    fpen.SetStartCap(Gdiplus::LineCapRound);
                    fpen.SetEndCap(Gdiplus::LineCapRound);
                    fpen.SetLineJoin(Gdiplus::LineJoinRound);
                    g.FillPath(&ffill, &fp);
                    g.DrawPath(&fpen, &fp);

                    Gdiplus::GraphicsPath lock_body;
                    build_round_rect_path(lock_body, cx - 4.0f * s, cy + 0.5f * s, 8.0f * s, 6.0f * s, 1.5f * s);
                    Gdiplus::SolidBrush lbr(Gdiplus::Color(255, 13, 71, 161));
                    g.FillPath(&lbr, &lock_body);
                    Gdiplus::Pen spen(Gdiplus::Color(255, 13, 71, 161), 1.4f * s);
                    spen.SetStartCap(Gdiplus::LineCapRound);
                    spen.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawArc(&spen, cx - 2.6f * s, cy - 3.2f * s, 5.2f * s, 5.2f * s, 180.0f, 180.0f);
                }

                SetBkMode(mem, TRANSPARENT);
                HFONT title_f = create_ui_font(app->dpi, 11, true);
                HFONT old_f = static_cast<HFONT>(SelectObject(mem, title_f ? title_f : app->bold_font));
                SetTextColor(mem, RGB(31, 35, 40));
                RECT trc{bx + S(16), by + S(84), bx + bw - S(16), by + S(112)};
                const wchar_t* title = filtered_empty ? L"未找到匹配的受管目录" : L"将要保护的目录拖放到此处";
                DrawTextW(mem, title, -1, &trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                SelectObject(mem, app->font);
                SetTextColor(mem, RGB(95, 99, 104));
                RECT src{bx + S(16), by + S(116), bx + bw - S(16), by + bh - S(12)};
                const wchar_t* sub = filtered_empty
                    ? L"尝试修改或清空上方搜索栏关键字"
                    : L"或点击左上角「+ 添加并锁定」 · 原目录内文件绝不被修改或加密";
                DrawTextW(mem, sub, -1, &src, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
                SelectObject(mem, old_f);
                if (title_f) DeleteObject(title_f);
            }

            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old_bmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, L"DirLockerApp");
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void update_empty_overlay_layout(WinApp& app) {
    if (!app.list || !app.empty_overlay) return;
    if (!app.visible_entry_indices.empty()) {
        ShowWindow(app.empty_overlay, SW_HIDE);
        return;
    }
    RECT lrc{};
    if (!GetClientRect(app.list, &lrc)) return;
    int hdr_h = scale_dpi(28, app.dpi);
    HWND hdr = ListView_GetHeader(app.list);
    if (hdr && IsWindow(hdr)) {
        RECT hrc{};
        if (GetWindowRect(hdr, &hrc) && (hrc.bottom - hrc.top) > 0) {
            hdr_h = hrc.bottom - hrc.top;
        }
    }
    int w = lrc.right - lrc.left;
    int h = (lrc.bottom - lrc.top) - hdr_h;
    if (w <= 0 || h <= 0) {
        ShowWindow(app.empty_overlay, SW_HIDE);
        return;
    }
    MoveWindow(app.empty_overlay, 0, hdr_h, w, h, FALSE);
    ShowWindow(app.empty_overlay, SW_SHOW);
    InvalidateRect(app.empty_overlay, nullptr, FALSE);
}

void adjust_list_columns(WinApp& app) {
    if (!app.list) return;
    auto S = [&](int px) { return scale_dpi(px, app.dpi); };
    int c1_w = S(76);
    int c2_w = S(112);
    int c3_w = S(156);
    // Shrink col 0 briefly before reading GetClientRect so stale horizontal widths cannot affect scrollbar metrics.
    ListView_SetColumnWidth(app.list, 0, S(120));
    ListView_SetColumnWidth(app.list, 1, c1_w);
    ListView_SetColumnWidth(app.list, 2, c2_w);
    ListView_SetColumnWidth(app.list, 3, c3_w);

    RECT lrc{};
    if (GetClientRect(app.list, &lrc)) {
        int client_w = lrc.right - lrc.left;
        int c0_w = client_w - c1_w - c2_w - c3_w;
        if (c0_w < S(180)) c0_w = S(180);
        ListView_SetColumnWidth(app.list, 0, c0_w);
    }
}

void layout_main_window(WinApp& app, int width, int height) {
    auto S = [&](int px) { return scale_dpi(px, app.dpi); };
    int pad = S(14);
    int top_y = S(12);
    int btn_h = S(30);
    int gap = S(8);

    // Right buttons: 设置 ▾, 帮助 ▾
    int rx = width - pad;
    int bw_help = S(76);
    rx -= bw_help;
    MoveWindow(GetDlgItem(app.hwnd, IDC_BTN_MENU_HELP), rx, top_y, bw_help, btn_h, TRUE);
    rx -= gap;

    int bw_settings = S(76);
    rx -= bw_settings;
    MoveWindow(GetDlgItem(app.hwnd, IDC_BTN_MENU_SETTINGS), rx, top_y, bw_settings, btn_h, TRUE);
    rx -= gap;

    // Left buttons + rounded search box
    int x = pad;
    int bw_add = S(124);
    MoveWindow(GetDlgItem(app.hwnd, IDC_BTN_ADD), x, top_y, bw_add, btn_h, TRUE);
    x += bw_add + gap;

    int bw_lock_all = S(164);
    MoveWindow(GetDlgItem(app.hwnd, IDC_BTN_LOCK_ALL), x, top_y, bw_lock_all, btn_h, TRUE);
    x += bw_lock_all + gap;

    int search_w = rx - x;
    if (search_w > S(240)) search_w = S(240);
    if (search_w < S(100)) search_w = S(100);
    app.search_rc = RECT{x, top_y, x + search_w, top_y + btn_h};
    int inner_h = S(17);
    MoveWindow(app.search_edit, x + S(10), top_y + (btn_h - inner_h) / 2, search_w - S(20), inner_h, TRUE);

    // Compact bottom status bar
    int status_h = S(20);
    int bottom_y = height - S(10) - status_h;
    app.status_line_y = bottom_y - S(6);
    int status_w = (width - 2 * pad) / 2;
    int policy_w = width - 2 * pad - status_w - gap;
    MoveWindow(app.status_label, pad, bottom_y, status_w, status_h, TRUE);
    MoveWindow(app.policy_label, pad + status_w + gap, bottom_y, policy_w, status_h, TRUE);

    int list_top = top_y + btn_h + S(10);
    int list_h = app.status_line_y - S(8) - list_top;
    if (list_h < S(140)) list_h = S(140);
    app.list_card_rc = RECT{pad, list_top, width - pad, list_top + list_h};
    MoveWindow(app.list, pad + S(3), list_top + S(2), (width - 2 * pad) - S(6), list_h - S(4), TRUE);

    adjust_list_columns(app);
    update_empty_overlay_layout(app);
    InvalidateRect(app.hwnd, nullptr, TRUE);
}

DWORD WINAPI PipeServerThreadProc(LPVOID param) {
    auto* app = static_cast<WinApp*>(param);
    while (app->pipe_running) {
        HANDLE pipe = CreateNamedPipeW(
            app->pipe_name.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && app->pipe_running) {
            std::string raw;
            char buf[1024];
            DWORD rd = 0;
            while (ReadFile(pipe, buf, sizeof(buf), &rd, nullptr) && rd > 0) {
                raw.append(buf, rd);
                if (raw.size() > 65536 || raw.find('\n') != std::string::npos) break;
            }
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
            JsonValue msg;
            if (JsonValue::parse(raw, msg) && msg.is_object()) {
                auto* cmd = new IpcCommand();
                cmd->action = msg.get("action").as_string_or("show");
                cmd->path = msg.get("path").as_string();
                if (!PostMessageW(app->hwnd, WM_APP_IPC, 0, reinterpret_cast<LPARAM>(cmd))) {
                    delete cmd;
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

void start_pipe_server(WinApp& app) {
    app.pipe_running = true;
    app.pipe_thread = CreateThread(nullptr, 0, PipeServerThreadProc, &app, 0, nullptr);
}

void stop_pipe_server(WinApp& app) {
    app.pipe_running = false;
    HANDLE dummy = CreateFileW(app.pipe_name.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) {
        CloseHandle(dummy);
    }
    if (app.pipe_thread) {
        WaitForSingleObject(app.pipe_thread, 2000);
        CloseHandle(app.pipe_thread);
        app.pipe_thread = nullptr;
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinApp* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<WinApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd = hwnd;
    } else {
        app = reinterpret_cast<WinApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (app && msg == app->taskbar_created_msg && app->taskbar_created_msg != 0) {
        add_or_update_tray(*app, false);
        return 0;
    }
    if (app && msg == app->quit_silent_msg && app->quit_silent_msg != 0) {
        app->really_quit = true;
        DestroyWindow(hwnd);
        return 0;
    }

    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            app->dpi = get_window_dpi(hwnd);
            auto S = [&](int px) { return scale_dpi(px, app->dpi); };
            app->font = create_ui_font(app->dpi, 9, false);
            app->bold_font = create_ui_font(app->dpi, 9, true);
            app->small_font = create_ui_font(app->dpi, 8, false);
            app->bg_brush = CreateSolidBrush(RGB(246, 248, 250));
            app->card_brush = CreateSolidBrush(RGB(255, 255, 255));
            app->taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
            app->quit_silent_msg = RegisterWindowMessageW(L"DirLocker_QuitSilent");

            app->folder_lock_icon = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LOCKED_FOLDER_ICON),
                IMAGE_ICON, S(28), S(28), LR_DEFAULTCOLOR));
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(L"dir", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
                app->folder_plain_icon = sfi.hIcon;
            }

            auto mk_btn = [&](const wchar_t* text, int id) {
                HWND h = CreateWindowExW(0, L"BUTTON", text,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 100, 28, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
                SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
                SetPropW(h, L"DirLockerApp", app);
                SetPropW(h, L"OrigBtnProc", reinterpret_cast<HANDLE>(
                    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HoverBtnSubclassProc))));
                return h;
            };

            mk_btn(L"＋ 添加并锁定", IDC_BTN_ADD);
            mk_btn(L"一键全锁 (Ctrl+Alt+L)", IDC_BTN_LOCK_ALL);
            app->search_edit = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 200, 20, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_EDIT)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(app->search_edit, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            SendMessageW(app->search_edit, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"搜索目录名称或路径..."));

            mk_btn(L"设置 ▾", IDC_BTN_MENU_SETTINGS);
            mk_btn(L"帮助 ▾", IDC_BTN_MENU_HELP);

            app->status_label = CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 200, 20, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_LABEL)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(app->status_label, WM_SETFONT,
                         reinterpret_cast<WPARAM>(app->small_font ? app->small_font : app->font), TRUE);

            app->policy_label = CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_NOPREFIX,
                0, 0, 200, 20, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_POLICY_LABEL)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(app->policy_label, WM_SETFONT,
                         reinterpret_cast<WPARAM>(app->small_font ? app->small_font : app->font), TRUE);

            app->list = CreateWindowExW(
                0, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | LVS_REPORT |
                    LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDRAWFIXED,
                0, 0, 400, 300, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_DIRS)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(app->list, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            ListView_SetExtendedListViewStyle(app->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetPropW(app->list, L"DirLockerApp", app);
            app->orig_list_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(app->list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListSubclassProc)));

            {
                static bool empty_cls_reg = false;
                const wchar_t* empty_cls = L"DirLockerEmptyOverlay";
                if (!empty_cls_reg) {
                    WNDCLASSEXW ewc{};
                    ewc.cbSize = sizeof(ewc);
                    ewc.lpfnWndProc = EmptyOverlayProc;
                    ewc.hInstance = GetModuleHandleW(nullptr);
                    ewc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                    ewc.lpszClassName = empty_cls;
                    RegisterClassExW(&ewc);
                    empty_cls_reg = true;
                }
                app->empty_overlay = CreateWindowExW(
                    0, empty_cls, L"", WS_CHILD,
                    0, 0, 100, 100, app->list, nullptr, GetModuleHandleW(nullptr), nullptr);
                if (app->empty_overlay) {
                    SetPropW(app->empty_overlay, L"DirLockerApp", app);
                }
            }

            app->list_tip = CreateWindowExW(
                WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (app->list_tip) {
                SendMessageW(app->list_tip, TTM_SETMAXTIPWIDTH, 0, 320);
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                ti.hwnd = hwnd;
                ti.uId = reinterpret_cast<UINT_PTR>(app->list);
                ti.lpszText = const_cast<LPWSTR>(L"");
                SendMessageW(app->list_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            }

            LVCOLUMNW col{};
            col.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
            col.fmt = LVCFMT_LEFT;
            col.pszText = const_cast<LPWSTR>(L"目录");
            col.cx = S(500);
            ListView_InsertColumn(app->list, 0, &col);
            col.fmt = LVCFMT_CENTER;
            col.pszText = const_cast<LPWSTR>(L"状态");
            col.cx = S(76);
            ListView_InsertColumn(app->list, 1, &col);
            col.fmt = LVCFMT_CENTER;
            col.pszText = const_cast<LPWSTR>(L"密码方式");
            col.cx = S(112);
            ListView_InsertColumn(app->list, 2, &col);
            col.fmt = LVCFMT_CENTER;
            col.pszText = const_cast<LPWSTR>(L"操作");
            col.cx = S(156);
            ListView_InsertColumn(app->list, 3, &col);

            DragAcceptFiles(hwnd, TRUE);
            add_or_update_tray(*app, false);
            app->hotkey_ok = RegisterHotKey(hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'L') != 0;
            WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
            SetTimer(hwnd, kIdleTimerId, 3000, nullptr);

            RECT rc{};
            GetClientRect(hwnd, &rc);
            layout_main_window(*app, rc.right - rc.left, rc.bottom - rc.top);
            refresh_ui(*app);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            if (mmi) {
                int dpi = (app && app->dpi) ? app->dpi : 96;
                mmi->ptMinTrackSize.x = scale_dpi(760, dpi);
                mmi->ptMinTrackSize.y = scale_dpi(440, dpi);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mis && mis->CtlID == IDC_LIST_DIRS) {
                mis->itemHeight = scale_dpi(54, (app && app->dpi) ? app->dpi : 96);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (dis && dis->CtlID == IDC_LIST_DIRS) {
                draw_dir_list_item(*app, dis);
                return TRUE;
            }
            if (dis && dis->CtlType == ODT_BUTTON) {
                draw_modern_button(*app, dis);
                return TRUE;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT cr{};
            GetClientRect(hwnd, &cr);
            if (app->bg_brush) FillRect(hdc, &cr, app->bg_brush);

            {
                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

                if (app->search_rc.right > app->search_rc.left) {
                    COLORREF bd = app->search_focused ? RGB(13, 71, 161) : RGB(208, 215, 222);
                    gp_draw_round_rect(g, app->search_rc, kWinCornerRadius, RGB(255, 255, 255), bd, app->dpi);
                }
                if (app->list_card_rc.right > app->list_card_rc.left) {
                    gp_draw_round_rect(g, app->list_card_rc, kWinCornerRadius, RGB(255, 255, 255),
                                       RGB(216, 222, 228), app->dpi);
                }
            }
            if (app->status_line_y > 0) {
                HPEN lpen = CreatePen(PS_SOLID, 1, RGB(216, 222, 228));
                HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, lpen));
                int pad = scale_dpi(14, app->dpi);
                MoveToEx(hdc, pad, app->status_line_y, nullptr);
                LineTo(hdc, cr.right - pad, app->status_line_y);
                SelectObject(hdc, old_pen);
                DeleteObject(lpen);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl == app->search_edit && app->card_brush) {
                SetBkColor(hdc, RGB(255, 255, 255));
                SetTextColor(hdc, RGB(31, 35, 40));
                return reinterpret_cast<LRESULT>(app->card_brush);
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(246, 248, 250));
            SetTextColor(hdc, RGB(95, 99, 104));
            return reinterpret_cast<LRESULT>(app->bg_brush ? app->bg_brush : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DROPFILES:
            do_drop_files(*app, reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_SHOWWINDOW:
            if (wp) {
                RECT rc{};
                if (GetClientRect(hwnd, &rc)) {
                    layout_main_window(*app, rc.right - rc.left, rc.bottom - rc.top);
                }
            }
            break;
        case WM_SIZE:
            layout_main_window(*app, LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_COMMAND: {
            if (LOWORD(wp) == IDC_SEARCH_EDIT) {
                if (HIWORD(wp) == EN_CHANGE) {
                    rebuild_list_view(*app);
                    return 0;
                }
                if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
                    app->search_focused = (HIWORD(wp) == EN_SETFOCUS);
                    InvalidateRect(hwnd, &app->search_rc, FALSE);
                    return 0;
                }
            }
            switch (LOWORD(wp)) {
                case IDC_BTN_ADD: do_add_and_lock(*app); break;
                case IDM_ROW_UNLOCK_OR_OPEN: do_unlock_selected(*app); break;
                case IDM_ROW_LOCK: do_lock_selected(*app); break;
                case IDC_BTN_LOCK_ALL:
                case IDM_TRAY_LOCK_ALL: lock_all_dirs(*app, L"一键全锁", true); break;
                case IDC_BTN_MENU_SETTINGS:
                    show_button_popup_menu(
                        hwnd, GetDlgItem(hwnd, IDC_BTN_MENU_SETTINGS),
                        {{IDM_TRAY_SETTINGS, L"安全与自动回锁设置..."},
                         {IDM_TRAY_MASTER_PW, L"修改主密码..."}});
                    break;
                case IDC_BTN_MENU_HELP:
                    show_button_popup_menu(
                        hwnd, GetDlgItem(hwnd, IDC_BTN_MENU_HELP),
                        {{IDC_BTN_RECOVERY, L"应急恢复说明..."},
                         {0, L""},
                         {IDM_TRAY_ABOUT, L"关于 目录密匣..."}});
                    break;
                case IDC_BTN_SETTINGS:
                case IDM_TRAY_SETTINGS: do_settings(*app); break;
                case IDC_BTN_MASTER_PW:
                case IDM_TRAY_MASTER_PW: do_change_master_pw(*app); break;
                case IDC_BTN_ABOUT:
                case IDM_TRAY_ABOUT: win_show_about_dialog(IsWindowVisible(hwnd) ? hwnd : nullptr); break;
                case IDC_BTN_DIR_PW:
                case IDM_ROW_DIR_PW: do_change_dir_password(*app); break;
                case IDC_BTN_EDIT_CUSTOM_PW:
                case IDM_ROW_EDIT_CUSTOM_PW: do_edit_custom_password(*app); break;
                case IDC_BTN_REMOVE:
                case IDM_ROW_REMOVE: do_remove_selected(*app); break;
                case IDC_BTN_RECOVERY: do_show_recovery(*app); break;
                case IDM_TRAY_OPEN: show_main_window(*app); break;
                case IDM_TRAY_EXIT: do_quit(*app); break;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm && nm->idFrom == IDC_LIST_DIRS) {
                if (nm->code == NM_CLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lp);
                    if (nmia) {
                        LVHITTESTINFO hti{};
                        hti.pt = nmia->ptAction;
                        ListView_SubItemHitTest(app->list, &hti);
                        if (hti.iItem >= 0 && hti.iItem < static_cast<int>(app->visible_entry_indices.size()) &&
                            hti.iSubItem == 3) {
                            const auto& entry = app->store->entries()[app->visible_entry_indices[static_cast<size_t>(hti.iItem)]];
                            RECT sub_rc{};
                            ListView_GetSubItemRect(app->list, hti.iItem, 3, LVIR_BOUNDS, &sub_rc);
                            int slot = hit_win_cell_slot(sub_rc, 3, entry.locked, nmia->ptAction, app->dpi);
                            if (slot > 0) {
                                ListView_SetItemState(app->list, hti.iItem,
                                                      LVIS_SELECTED | LVIS_FOCUSED,
                                                      LVIS_SELECTED | LVIS_FOCUSED);
                                if (slot == 1) {
                                    if (entry.locked) do_unlock_selected(*app);
                                    else do_lock_selected(*app);
                                } else if (slot == 2 && !entry.locked) {
                                    open_in_explorer(entry.path);
                                } else if (slot == 3) {
                                    WinCellLayout lay = compute_win_cell_layout(sub_rc, 3, entry.locked, app->dpi);
                                    POINT pt{lay.more_btn.right, lay.more_btn.bottom + scale_dpi(2, app->dpi)};
                                    ClientToScreen(app->list, &pt);
                                    show_row_context_menu(*app, hti.iItem, pt,
                                                          TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, true);
                                }
                            }
                        }
                    }
                } else if (nm->code == NM_DBLCLK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lp);
                    if (nmia) {
                        LVHITTESTINFO hti{};
                        hti.pt = nmia->ptAction;
                        ListView_SubItemHitTest(app->list, &hti);
                        if (hti.iItem >= 0 && hti.iItem < static_cast<int>(app->visible_entry_indices.size())) {
                            if (hti.iSubItem == 3) return 0;
                            ListView_SetItemState(app->list, hti.iItem,
                                                  LVIS_SELECTED | LVIS_FOCUSED,
                                                  LVIS_SELECTED | LVIS_FOCUSED);
                            const auto& entry = app->store->entries()[app->visible_entry_indices[static_cast<size_t>(hti.iItem)]];
                            if (hti.iSubItem == 1) {
                                if (entry.locked) do_unlock_selected(*app);
                                else do_lock_selected(*app);
                                return 0;
                            }
                            if (hti.iSubItem == 2) {
                                do_change_dir_password(*app);
                                return 0;
                            }
                        }
                    }
                    do_unlock_selected(*app);
                } else if (nm->code == NM_RCLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lp);
                    int vis_row = ListView_GetNextItem(app->list, -1, LVNI_SELECTED);
                    if (nmia && nmia->iItem >= 0) vis_row = nmia->iItem;
                    if (vis_row >= 0) {
                        ListView_SetItemState(app->list, vis_row,
                                              LVIS_SELECTED | LVIS_FOCUSED,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                        POINT pt{};
                        GetCursorPos(&pt);
                        show_row_context_menu(*app, vis_row, pt, TPM_RIGHTBUTTON, false);
                    }
                }
            }
            return 0;
        }
        case WM_HOTKEY:
            if (static_cast<int>(wp) == kHotkeyId) {
                lock_all_dirs(*app, L"快捷键 Ctrl+Alt+L", true);
            }
            return 0;
        case WM_TIMER:
            if (wp == kIdleTimerId) {
                int mins = app->store->settings().auto_relock_minutes;
                if (mins <= 0) {
                    app->idle_triggered = false;
                } else {
                    LASTINPUTINFO lii{};
                    lii.cbSize = sizeof(lii);
                    if (GetLastInputInfo(&lii)) {
                        DWORD elapsed = GetTickCount() - lii.dwTime;
                        DWORD threshold = static_cast<DWORD>(mins) * 60000UL;
                        if (elapsed >= threshold) {
                            if (!app->idle_triggered) {
                                app->idle_triggered = true;
                                lock_all_dirs(*app, L"闲置超时自动回锁", false);
                            }
                        } else {
                            app->idle_triggered = false;
                        }
                    }
                }
            }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK && app->store->settings().relock_on_screen_lock) {
                lock_all_dirs(*app, L"锁屏自动回锁", false);
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND && app->store->settings().relock_on_screen_lock) {
                lock_all_dirs(*app, L"系统休眠前自动回锁", false);
            }
            return TRUE;
        case WM_APP_TRAY: {
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                else show_main_window(*app);
            } else if (lp == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"打开主界面");
                AppendMenuW(menu, MF_STRING, IDM_TRAY_LOCK_ALL, L"一键全部锁定 (Ctrl+Alt+L)");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"安全设置");
                AppendMenuW(menu, MF_STRING, IDM_TRAY_MASTER_PW, L"修改主密码");
                AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, L"关于 目录密匣");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出 目录密匣");
                POINT pt{};
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
            }
            return 0;
        }
        case WM_APP_IPC: {
            std::unique_ptr<IpcCommand> cmd(reinterpret_cast<IpcCommand*>(lp));
            if (cmd) handle_command(*app, cmd->action, cmd->path);
            return 0;
        }
        case WM_CLOSE:
            if (!app->really_quit) {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(hwnd, kIdleTimerId);
            WTSUnRegisterSessionNotification(hwnd);
            if (app->hotkey_ok) UnregisterHotKey(hwnd, kHotkeyId);
            remove_tray(*app);
            stop_pipe_server(*app);
            if (app->font) DeleteObject(app->font);
            if (app->bold_font) DeleteObject(app->bold_font);
            if (app->small_font) DeleteObject(app->small_font);
            if (app->bg_brush) DeleteObject(app->bg_brush);
            if (app->card_brush) DeleteObject(app->card_brush);
            if (app->folder_lock_icon) DestroyIcon(app->folder_lock_icon);
            if (app->folder_plain_icon) DestroyIcon(app->folder_plain_icon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_cli_install() {
    std::string self = get_self_exe_path();
    std::string err;
    if (!win_install_integration(self, true, true, &err)) {
        MessageBoxW(nullptr, utf8_to_wide(err).c_str(), L"安装失败", MB_ICONERROR);
        return 1;
    }
    MessageBoxW(nullptr,
                L"目录密匣 安装成功！\n\n"
                L"1. 已添加资源管理器右键菜单「使用 目录密匣 锁定/解锁」（Win11 请在“显示更多选项”中查看）\n"
                L"2. 已注册无角标蓝白锁标加密目录图标（.dlock，隐藏扩展名）\n"
                L"3. 已添加开机自启与开始菜单快捷方式",
                L"目录密匣 安装完成", MB_ICONINFORMATION);
    return 0;
}

int confirm_uninstall_locked_dirs() {
    StateStore store(get_windows_state_path());
    store.load(nullptr);
    int locked_cnt = 0;
    for (const auto& e : store.entries()) {
        if (e.locked) ++locked_cnt;
    }
    if (locked_cnt > 0) {
        std::wstring warn = L"检测到仍有 " + std::to_wstring(locked_cnt) +
                            L" 个目录处于锁定状态！\n强烈建议先在目录密匣中解锁所有目录后再卸载。\n\n"
                            L"如果继续卸载，文件不会丢失，可通过应急说明手动恢复。确定继续卸载吗？";
        if (MessageBoxW(nullptr, warn.c_str(), L"卸载确认", MB_ICONWARNING | MB_YESNO) != IDYES) {
            return 1;
        }
    }
    return 0;
}

int run_cli_uninstall() {
    if (confirm_uninstall_locked_dirs() != 0) return 0;
    std::string err;
    if (!win_uninstall_integration(&err)) {
        MessageBoxW(nullptr, utf8_to_wide(err).c_str(), L"卸载失败", MB_ICONERROR);
        return 1;
    }
    MessageBoxW(nullptr, L"目录密匣右键菜单、自启和快捷方式已卸载。", L"卸载完成", MB_ICONINFORMATION);
    return 0;
}

}  // namespace
}  // namespace dirlocker

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    using namespace dirlocker;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::string action = "show";
    std::string path;
    bool tray_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = wide_to_utf8(argv[i]);
        if (a == "--install") {
            LocalFree(argv);
            int rc = run_cli_install();
            CoUninitialize();
            return rc;
        }
        if (a == "--register" || a == "--register-no-autostart") {
            LocalFree(argv);
            bool with_autostart = (a == "--register");
            int rc = win_install_integration(get_self_exe_path(), with_autostart, false, nullptr) ? 0 : 1;
            CoUninitialize();
            return rc;
        }
        if (a == "--uninstall") {
            LocalFree(argv);
            int rc = run_cli_uninstall();
            CoUninitialize();
            return rc;
        }
        if (a == "--unregister") {
            LocalFree(argv);
            int rc = win_uninstall_integration(nullptr) ? 0 : 1;
            CoUninitialize();
            return rc;
        }
        if (a == "--uninstall-confirm") {
            LocalFree(argv);
            int rc = confirm_uninstall_locked_dirs();
            CoUninitialize();
            return rc;
        }
        if (a == "--quit-silent") {
            LocalFree(argv);
            try_forward_to_running_instance(build_pipe_name(), "quit-silent", "");
            CoUninitialize();
            return 0;
        }
        if (a == "--tray") {
            tray_only = true;
            action = "none";
        } else if (a == "--show") {
            action = "show";
        } else if (a == "--lock-all") {
            action = "lock-all";
        } else if (a == "--unlock-file" && i + 1 < argc) {
            action = "unlock-file";
            path = wide_to_utf8(argv[++i]);
        } else if (a == "--unlock-id" && i + 1 < argc) {
            action = "unlock-id";
            path = wide_to_utf8(argv[++i]);
        } else if ((a == "--lock" || a == "--unlock" || a == "--toggle") && i + 1 < argc) {
            action = a.substr(2);
            path = wide_to_utf8(argv[++i]);
        }
    }
    if (argv) LocalFree(argv);

    std::wstring pipe_name = build_pipe_name();
    if (try_forward_to_running_instance(pipe_name, action == "none" ? "show" : action, path)) {
        CoUninitialize();
        return 0;
    }

    WinApp app;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    Gdiplus::GdiplusStartup(&app.gdiplus_token, &gdiplus_input, nullptr);
    app.pipe_name = pipe_name;
    app.icon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON));
    app.self_exe = get_self_exe_path();
    win_ensure_dlock_association(app.self_exe);
    std::string state_path = get_windows_state_path();
    app.state_dir = parent_dir_of(state_path);
    app.extra_protected.push_back(app.state_dir);
    app.extra_protected.push_back(app.state_dir + "\\Vault");
    app.extra_protected.push_back(parent_dir_of(get_default_install_exe_path()));
    app.extra_protected.push_back(parent_dir_of(app.self_exe));

    app.store = std::make_unique<StateStore>(state_path);
    std::string load_err;
    if (!app.store->load(&load_err)) {
        MessageBoxW(nullptr, utf8_to_wide("加载状态文件失败: " + load_err).c_str(),
                    L"目录密匣错误", MB_ICONERROR);
    }

    const wchar_t* cls_name = L"DirLockerMainWnd";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hinst;
    wc.hIcon = app.icon;
    wc.hIconSm = app.icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls_name;
    RegisterClassExW(&wc);

    int dpi = get_window_dpi(nullptr);
    int w = scale_dpi(920, dpi);
    int h = scale_dpi(560, dpi);
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        0, cls_name, L"目录密匣 v" DIRLOCKER_VERSION_WSTR,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (sx - w) / 2, (sy - h) / 2, w, h,
        nullptr, nullptr, hinst, &app);
    if (!hwnd) {
        if (app.gdiplus_token) Gdiplus::GdiplusShutdown(app.gdiplus_token);
        CoUninitialize();
        return 1;
    }

    start_pipe_server(app);

    if (!tray_only && action == "show") {
        show_main_window(app);
        ensure_master_password(app, app.hwnd);
    } else if (action != "none" && action != "show") {
        auto* cmd = new IpcCommand{action, path};
        PostMessageW(app.hwnd, WM_APP_IPC, 0, reinterpret_cast<LPARAM>(cmd));
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app.gdiplus_token) Gdiplus::GdiplusShutdown(app.gdiplus_token);
    CoUninitialize();
    return 0;
}
