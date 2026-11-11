#include "win_registry.hpp"

#include "../core/path_guard.hpp"
#include "resource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <vector>

namespace dirlocker {

namespace {

std::wstring get_known_csidl_dir(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return std::wstring(buf);
    }
    return {};
}

bool get_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* value_name, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD cb = 0;
    bool ok = false;
    if (RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && cb >= sizeof(wchar_t)) {
        std::wstring buf(cb / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, value_name, nullptr, &type,
                             reinterpret_cast<BYTE*>(&buf[0]), &cb) == ERROR_SUCCESS) {
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            out = std::move(buf);
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

bool set_reg_sz(HKEY root, const wchar_t* subkey, const wchar_t* value_name, const std::wstring& data) {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                 KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return false;
    DWORD cb = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    st = RegSetValueExW(key, value_name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(data.c_str()), cb);
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool can_write_hklm() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes", 0,
                      KEY_CREATE_SUB_KEY | KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

void delete_reg_value_safe(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, value_name);
        RegCloseKey(key);
    }
}

bool write_dlock_association_to(HKEY hive_root, const std::wstring& wexe) {
    std::wstring want_progid = L"DirLocker.LockedDir";
    std::wstring want_icon = wexe + L",-" + std::to_wstring(IDI_LOCKED_FOLDER_ICON);
    std::wstring want_cmd = L"\"" + wexe + L"\" --unlock-file \"%1\"";

    bool ok =
        set_reg_sz(hive_root, L"Software\\Classes\\.dlock", nullptr, want_progid) &&
        set_reg_sz(hive_root, L"Software\\Classes\\.dlock\\OpenWithProgids", want_progid.c_str(), L"") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir", nullptr, L"目录密匣加密目录") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir", L"FriendlyTypeName", L"目录密匣加密目录") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir", L"NeverShowExt", L"") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir\\DefaultIcon", nullptr, want_icon) &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir\\shell", nullptr, L"open") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir\\shell\\open", nullptr, L"使用 目录密匣 解锁打开") &&
        set_reg_sz(hive_root, L"Software\\Classes\\DirLocker.LockedDir\\shell\\open\\command", nullptr, want_cmd);

    if (ok) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    }
    return ok;
}

bool create_shortcut_lnk(const std::wstring& lnk_path,
                         const std::wstring& target_exe,
                         const std::wstring& args,
                         const std::wstring& desc) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) return false;

    psl->SetPath(target_exe.c_str());
    psl->SetArguments(args.c_str());
    psl->SetDescription(desc.c_str());
    psl->SetIconLocation(target_exe.c_str(), 0);

    size_t slash = target_exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring work_dir = target_exe.substr(0, slash);
        psl->SetWorkingDirectory(work_dir.c_str());
    }

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(lnk_path.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

}  // namespace

std::string get_self_exe_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) {
            return wide_to_utf8(std::wstring(buf.data(), n));
        }
        buf.resize(buf.size() * 2);
    }
}

std::string get_windows_state_path() {
    std::wstring local = get_known_csidl_dir(CSIDL_LOCAL_APPDATA);
    if (local.empty()) local = L".";
    return wide_to_utf8(local + L"\\DirLocker\\state.json");
}

std::string get_default_install_exe_path() {
    std::wstring pf = get_known_csidl_dir(CSIDL_PROGRAM_FILES);
    if (!pf.empty()) {
        return wide_to_utf8(pf + L"\\DirLocker\\dirlocker.exe");
    }
    return "C:\\Program Files\\DirLocker\\dirlocker.exe";
}

bool win_ensure_dlock_association(const std::string& exe_path) {
    std::wstring wexe = utf8_to_wide(exe_path);
    if (wexe.empty()) return false;

    std::wstring want_progid = L"DirLocker.LockedDir";
    std::wstring want_icon = wexe + L",-" + std::to_wstring(IDI_LOCKED_FOLDER_ICON);
    std::wstring want_cmd = L"\"" + wexe + L"\" --unlock-file \"%1\"";

    // Check merged HKEY_CLASSES_ROOT first so HKLM system-wide install isn't needlessly duplicated in HKCU.
    std::wstring cur_progid, cur_icon, cur_cmd, cur_never;
    bool already_ok =
        get_reg_sz(HKEY_CLASSES_ROOT, L".dlock", nullptr, cur_progid) &&
        cur_progid == want_progid &&
        get_reg_sz(HKEY_CLASSES_ROOT, L"DirLocker.LockedDir", L"NeverShowExt", cur_never) &&
        get_reg_sz(HKEY_CLASSES_ROOT, L"DirLocker.LockedDir\\DefaultIcon", nullptr, cur_icon) &&
        _wcsicmp(cur_icon.c_str(), want_icon.c_str()) == 0 &&
        get_reg_sz(HKEY_CLASSES_ROOT, L"DirLocker.LockedDir\\shell\\open\\command", nullptr, cur_cmd) &&
        _wcsicmp(cur_cmd.c_str(), want_cmd.c_str()) == 0;
    if (already_ok) return true;

    HKEY target_hive = can_write_hklm() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    if (target_hive == HKEY_LOCAL_MACHINE) {
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.dlock");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\DirLocker.LockedDir");
    }
    return write_dlock_association_to(target_hive, wexe);
}

bool win_install_integration(const std::string& exe_path,
                             bool with_autostart,
                             bool with_start_menu,
                             std::string* err) {
    std::wstring wexe = utf8_to_wide(exe_path);
    if (wexe.empty()) {
        if (err) *err = "程序路径为空";
        return false;
    }

    bool machine_scope = can_write_hklm();
    HKEY hive = machine_scope ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

    const wchar_t* menu_key = L"Software\\Classes\\Directory\\shell\\DirLocker";
    const wchar_t* cmd_key = L"Software\\Classes\\Directory\\shell\\DirLocker\\command";

    if (machine_scope) {
        // Clear legacy per-user overrides so machine-wide Program Files keys take effect cleanly.
        RegDeleteTreeW(HKEY_CURRENT_USER, menu_key);
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.dlock");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\DirLocker.LockedDir");
        delete_reg_value_safe(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"DirLocker");
    }

    if (!set_reg_sz(hive, menu_key, nullptr, L"使用 目录密匣 锁定/解锁") ||
        !set_reg_sz(hive, menu_key, L"Icon", L"\"" + wexe + L"\",0") ||
        !set_reg_sz(hive, cmd_key, nullptr, L"\"" + wexe + L"\" --toggle \"%1\"")) {
        if (err) *err = "写入资源管理器右键菜单注册表失败";
        return false;
    }

    if (!write_dlock_association_to(hive, wexe)) {
        if (err) *err = "注册 .dlock 加密目录关联失败";
        return false;
    }

    const wchar_t* run_subkey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (with_autostart) {
        if (!set_reg_sz(hive, run_subkey, L"DirLocker", L"\"" + wexe + L"\" --tray")) {
            if (err) *err = "写入开机自启注册表失败";
            return false;
        }
    } else {
        delete_reg_value_safe(hive, run_subkey, L"DirLocker");
    }

    if (with_start_menu) {
        std::wstring programs = get_known_csidl_dir(machine_scope ? CSIDL_COMMON_PROGRAMS : CSIDL_PROGRAMS);
        if (!programs.empty()) {
            create_shortcut_lnk(programs + L"\\目录密匣.lnk", wexe, L"--show", L"目录密匣");
        }
    }

    return true;
}

bool win_uninstall_integration(std::string* err) {
    LSTATUS st_cu = RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\DirLocker");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.dlock");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\DirLocker.LockedDir");
    delete_reg_value_safe(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"DirLocker");

    bool ok = (st_cu == ERROR_SUCCESS || st_cu == ERROR_FILE_NOT_FOUND);

    if (can_write_hklm()) {
        LSTATUS st_lm = RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\Directory\\shell\\DirLocker");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\.dlock");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\DirLocker.LockedDir");
        delete_reg_value_safe(HKEY_LOCAL_MACHINE,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"DirLocker");
        if (st_lm != ERROR_SUCCESS && st_lm != ERROR_FILE_NOT_FOUND) ok = false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    for (int csidl : {CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS}) {
        std::wstring programs = get_known_csidl_dir(csidl);
        if (!programs.empty()) {
            DeleteFileW((programs + L"\\DirLocker 目录锁.lnk").c_str());
            DeleteFileW((programs + L"\\目录密盾.lnk").c_str());
            DeleteFileW((programs + L"\\目录密匣.lnk").c_str());
        }
    }

    if (!ok && err) *err = "删除右键菜单注册表失败";
    return ok;
}

}  // namespace dirlocker
