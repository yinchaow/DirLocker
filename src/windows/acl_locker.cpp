#include "acl_locker.hpp"

#include "../core/json_mini.hpp"
#include "../core/path_guard.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cctype>
#include <cstring>
#include <vector>

namespace dirlocker {

namespace {

constexpr DWORD kLockDenyMask =
    FILE_LIST_DIRECTORY |
    FILE_ADD_FILE |
    FILE_ADD_SUBDIRECTORY |
    FILE_READ_EA |
    FILE_WRITE_EA |
    FILE_TRAVERSE |
    FILE_DELETE_CHILD |
    DELETE |
    FILE_WRITE_ATTRIBUTES;

// Blocks clueless delete / rename / overwrite / clear-readonly on .dlock stubs while keeping READ_DATA & WRITE_DAC.
constexpr DWORD kStubDenyMask =
    DELETE |
    FILE_WRITE_DATA |
    FILE_APPEND_DATA |
    FILE_WRITE_ATTRIBUTES |
    FILE_WRITE_EA;

std::string format_win_err(const std::string& prefix, DWORD err) {
    if (err == ERROR_ACCESS_DENIED) {
        return prefix + ": 权限不足（请尝试以管理员身份运行 目录密匣）";
    }
    return prefix + " (Win32 错误码: " + std::to_string(err) + ")";
}

bool create_everyone_sid(std::vector<uint8_t>& sid_buf) {
    DWORD size = SECURITY_MAX_SID_SIZE;
    sid_buf.resize(size);
    if (!CreateWellKnownSid(WinWorldSid, nullptr, sid_buf.data(), &size)) {
        sid_buf.clear();
        return false;
    }
    sid_buf.resize(size);
    return true;
}

bool resolve_target_sid(const std::string& sid_str, std::vector<uint8_t>& sid_buf) {
    if (!sid_str.empty()) {
        PSID raw_sid = nullptr;
        std::wstring wsid = utf8_to_wide(sid_str);
        if (ConvertStringSidToSidW(wsid.c_str(), &raw_sid) && raw_sid) {
            DWORD len = GetLengthSid(raw_sid);
            sid_buf.resize(len);
            std::memcpy(sid_buf.data(), raw_sid, len);
            LocalFree(raw_sid);
            return true;
        }
    }
    return create_everyone_sid(sid_buf);
}

bool has_deny_ace_mask(PACL dacl, PSID target_sid, DWORD deny_mask) {
    if (!dacl || !target_sid) return false;
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
        return false;
    }
    for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID ace_ptr = nullptr;
        if (!GetAce(dacl, i, &ace_ptr) || !ace_ptr) continue;
        auto* hdr = static_cast<PACE_HEADER>(ace_ptr);
        if (hdr->AceType != ACCESS_DENIED_ACE_TYPE) continue;
        auto* deny_ace = static_cast<PACCESS_DENIED_ACE>(ace_ptr);
        if (EqualSid(&deny_ace->SidStart, target_sid) &&
            (deny_ace->Mask & deny_mask) == deny_mask &&
            (hdr->AceFlags & INHERITED_ACE) == 0) {
            return true;
        }
    }
    return false;
}

bool apply_deny_ace(const std::string& path, DWORD deny_mask, std::string& used_sid, std::string* err) {
    std::vector<uint8_t> sid_buf;
    if (used_sid.empty()) used_sid = "S-1-1-0";
    if (!resolve_target_sid(used_sid, sid_buf)) {
        if (err) *err = "无法构造安全标识符 (SID)";
        return false;
    }
    PSID target_sid = sid_buf.data();

    std::wstring wpath = utf8_to_wide(path);
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(wpath.c_str(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = format_win_err("读取 ACL 失败", rc);
        return false;
    }

    if (has_deny_ace_mask(old_dacl, target_sid, deny_mask)) {
        LocalFree(sd);
        return true;
    }

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = deny_mask;
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(target_sid);

    PACL new_dacl = nullptr;
    rc = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
    if (rc != ERROR_SUCCESS) {
        LocalFree(sd);
        if (err) *err = format_win_err("构建保护 ACL 失败", rc);
        return false;
    }

    rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION,
                               nullptr, nullptr, new_dacl, nullptr);
    LocalFree(new_dacl);
    LocalFree(sd);

    if (rc != ERROR_SUCCESS) {
        if (err) *err = format_win_err("写入保护权限失败", rc);
        return false;
    }
    return true;
}

bool remove_deny_ace(const std::string& path, DWORD deny_mask, const std::string& used_sid, std::string* err) {
    std::vector<uint8_t> sid_buf;
    if (!resolve_target_sid(used_sid.empty() ? "S-1-1-0" : used_sid, sid_buf)) {
        if (err) *err = "无法解析安全标识符 (SID)";
        return false;
    }
    PSID target_sid = sid_buf.data();

    std::wstring wpath = utf8_to_wide(path);
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(wpath.c_str(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = format_win_err("读取 ACL 失败", rc);
        return false;
    }

    if (!old_dacl) {
        LocalFree(sd);
        return true;
    }

    ACL_SIZE_INFORMATION size_info{};
    if (!GetAclInformation(old_dacl, &size_info, sizeof(size_info), AclSizeInformation)) {
        LocalFree(sd);
        if (err) *err = "获取 ACL 大小信息失败";
        return false;
    }

    ACL_REVISION_INFORMATION rev_info{};
    rev_info.AclRevision = ACL_REVISION;
    GetAclInformation(old_dacl, &rev_info, sizeof(rev_info), AclRevisionInformation);

    bool removed_any = false;
    DWORD alloc_size = size_info.AclBytesInUse > sizeof(ACL) ? size_info.AclBytesInUse : sizeof(ACL) + 64;
    auto* new_dacl = static_cast<PACL>(LocalAlloc(LPTR, alloc_size));
    if (!new_dacl) {
        LocalFree(sd);
        if (err) *err = "内存不足";
        return false;
    }

    if (!InitializeAcl(new_dacl, alloc_size, rev_info.AclRevision)) {
        LocalFree(new_dacl);
        LocalFree(sd);
        if (err) *err = "初始化新 ACL 失败";
        return false;
    }

    for (DWORD i = 0; i < size_info.AceCount; ++i) {
        LPVOID ace_ptr = nullptr;
        if (!GetAce(old_dacl, i, &ace_ptr) || !ace_ptr) continue;
        auto* hdr = static_cast<PACE_HEADER>(ace_ptr);

        bool should_remove = false;
        if (hdr->AceType == ACCESS_DENIED_ACE_TYPE && (hdr->AceFlags & INHERITED_ACE) == 0) {
            auto* deny_ace = static_cast<PACCESS_DENIED_ACE>(ace_ptr);
            if (EqualSid(&deny_ace->SidStart, target_sid) && deny_ace->Mask == deny_mask) {
                should_remove = true;
            }
        }
        if (should_remove) {
            removed_any = true;
            continue;
        }
        if (!AddAce(new_dacl, rev_info.AclRevision, MAXDWORD, ace_ptr, hdr->AceSize)) {
            LocalFree(new_dacl);
            LocalFree(sd);
            if (err) *err = "复制原有权限条目失败";
            return false;
        }
    }

    if (removed_any) {
        rc = SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION,
                                   nullptr, nullptr, new_dacl, nullptr);
    } else {
        rc = ERROR_SUCCESS;
    }

    LocalFree(new_dacl);
    LocalFree(sd);

    if (rc != ERROR_SUCCESS) {
        if (err) *err = format_win_err("恢复目录原始权限失败", rc);
        return false;
    }
    return true;
}

std::string parent_of_win_path(const std::string& p) {
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return {};
    if (pos == 2 && p.size() >= 3 && p[1] == ':') return p.substr(0, 3);
    return p.substr(0, pos);
}

bool ensure_hidden_dir_unlocked(const std::string& dir_path) {
    std::wstring wdir = utf8_to_wide(dir_path);
    for (size_t i = 0; i < wdir.size(); ++i) {
        if (wdir[i] == L'\\' || wdir[i] == L'/') {
            if (i == 2 && wdir[1] == L':') continue;
            if (i <= 1) continue;
            CreateDirectoryW(wdir.substr(0, i).c_str(), nullptr);
        }
    }
    if (!CreateDirectoryW(wdir.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    remove_deny_ace(dir_path, kLockDenyMask, "S-1-1-0", nullptr);
    SetFileAttributesW(wdir.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    return true;
}

bool is_dir_empty_win(const std::string& dir_path) {
    std::wstring pat = utf8_to_wide(dir_path);
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/') pat.push_back(L'\\');
    pat.push_back(L'*');

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool empty = true;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

void seal_vault_root_if_nonempty(const std::string& vault_root, bool cleanup_if_empty) {
    if (vault_root.empty()) return;
    remove_deny_ace(vault_root, kLockDenyMask, "S-1-1-0", nullptr);
    if (cleanup_if_empty && is_dir_empty_win(vault_root)) {
        std::wstring wroot = utf8_to_wide(vault_root);
        SetFileAttributesW(wroot.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        if (RemoveDirectoryW(wroot.c_str())) {
            return;
        }
    }
    if (!is_dir_empty_win(vault_root)) {
        SetFileAttributesW(utf8_to_wide(vault_root).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        std::string sid = "S-1-1-0";
        apply_deny_ace(vault_root, kLockDenyMask, sid, nullptr);
    }
}

bool delete_protected_file(const std::string& file_path) {
    std::wstring wpath = utf8_to_wide(file_path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    remove_deny_ace(file_path, kStubDenyMask, "S-1-1-0", nullptr);
    SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(wpath.c_str()) != 0;
}

bool create_locked_dlock_stub(const std::string& dlock_path, const DirEntry& entry) {
    delete_protected_file(dlock_path);

    JsonValue doc = JsonValue::object();
    doc.set("app", "目录密匣");
    doc.set("version", 1);
    doc.set("id", entry.id);
    doc.set("path", entry.path);
    doc.set("tip", "此文件为“目录密匣”加密目录入口，双击输入密码即可解锁恢复，请勿手工删除。");
    std::string body = doc.dump(2) + "\n";

    std::wstring wpath = utf8_to_wide(dlock_path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    size_t written = 0;
    bool ok = true;
    while (written < body.size()) {
        DWORD out = 0;
        if (!WriteFile(h, body.data() + written, static_cast<DWORD>(body.size() - written), &out, nullptr) ||
            out == 0) {
            ok = false;
            break;
        }
        written += out;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(wpath.c_str());
        return false;
    }

    SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_READONLY);
    std::string sid = "S-1-1-0";
    apply_deny_ace(dlock_path, kStubDenyMask, sid, nullptr);
    return true;
}

bool choose_same_volume_vault(const std::string& orig_path,
                              const std::string& state_dir,
                              const std::string& entry_id,
                              std::string& out_vault) {
    if (orig_path.size() < 3 || orig_path[1] != ':') {
        return false;  // UNC or non-drive path
    }
    char orig_drv = static_cast<char>(std::toupper(static_cast<unsigned char>(orig_path[0])));
    char state_drv = (state_dir.size() >= 2 && state_dir[1] == ':')
        ? static_cast<char>(std::toupper(static_cast<unsigned char>(state_dir[0])))
        : '\0';

    if (orig_drv == state_drv && !state_dir.empty()) {
        std::string vroot = state_dir + "\\Vault";
        if (ensure_hidden_dir_unlocked(vroot)) {
            out_vault = vroot + "\\" + entry_id;
            return true;
        }
    }

    std::string drv_vroot = std::string(1, orig_drv) + ":\\$目录密匣_重要数据保护区_切勿删除";
    if (ensure_hidden_dir_unlocked(drv_vroot)) {
        out_vault = drv_vroot + "\\" + entry_id;
        return true;
    }

    std::string parent = parent_of_win_path(orig_path);
    if (!parent.empty()) {
        if (parent.back() != '\\') parent.push_back('\\');
        out_vault = parent + ".dirvault_" + entry_id;
        return true;
    }
    return false;
}

void notify_shell_parent_updated(const std::string& path) {
    std::string parent = parent_of_win_path(path);
    if (!parent.empty()) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                       utf8_to_wide(parent).c_str(), nullptr);
    }
}

}  // namespace

std::string get_current_user_sid_string() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return {};
    }
    std::vector<uint8_t> buf(needed);
    if (!GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    auto* user = reinterpret_cast<PTOKEN_USER>(buf.data());
    LPWSTR wsid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &wsid) || !wsid) {
        return {};
    }
    std::string out = wide_to_utf8(wsid);
    LocalFree(wsid);
    return out;
}

bool win_lock_directory(const std::string& path, std::string& used_sid, std::string* err) {
    return apply_deny_ace(path, kLockDenyMask, used_sid, err);
}

bool win_unlock_directory(const std::string& path, const std::string& used_sid, std::string* err) {
    return remove_deny_ace(path, kLockDenyMask, used_sid, err);
}

bool win_is_directory_locked(const std::string& path, const std::string& used_sid) {
    std::vector<uint8_t> sid_buf;
    if (!resolve_target_sid(used_sid.empty() ? "S-1-1-0" : used_sid, sid_buf)) {
        return false;
    }
    std::wstring wpath = utf8_to_wide(path);
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(wpath.c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &old_dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    bool locked = has_deny_ace_mask(old_dacl, sid_buf.data(), kLockDenyMask);
    LocalFree(sd);
    return locked;
}

bool win_read_dlock_file(const std::string& dlock_path, std::string& out_id, std::string& out_orig_path) {
    out_id.clear();
    out_orig_path.clear();

    HANDLE h = CreateFileW(utf8_to_wide(dlock_path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) {
        CloseHandle(h);
        return false;
    }
    std::string raw(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD rd = 0;
    BOOL ok = ReadFile(h, &raw[0], static_cast<DWORD>(raw.size()), &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd == 0) return false;
    raw.resize(rd);

    JsonValue root;
    if (!JsonValue::parse(raw, root) || !root.is_object()) return false;
    out_id = root.get("id").as_string();
    out_orig_path = root.get("path").as_string();
    return !out_id.empty() || !out_orig_path.empty();
}

bool win_repair_locked_entry_stub(const DirEntry& entry) {
    if (!entry.locked || entry.in_place_acl || entry.vault_path.empty()) return false;
    std::wstring worig = utf8_to_wide(entry.path);
    std::wstring wvault = utf8_to_wide(entry.vault_path);
    if (GetFileAttributesW(wvault.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (GetFileAttributesW(worig.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    std::string dlock_path = entry.path + ".dlock";
    if (GetFileAttributesW(utf8_to_wide(dlock_path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;  // already present
    }

    if (!create_locked_dlock_stub(dlock_path, entry)) return false;

    // Remove legacy .lnk from earlier test versions if present
    std::wstring wlnk = utf8_to_wide(entry.path + ".lnk");
    if (GetFileAttributesW(wlnk.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(wlnk.c_str());
    }
    notify_shell_parent_updated(entry.path);
    return true;
}

bool win_smart_lock(DirEntry& entry, const std::string&, const std::string& state_dir,
                    bool allow_in_place_fallback, std::string* err) {
    std::wstring worig = utf8_to_wide(entry.path);
    DWORD orig_attrs = GetFileAttributesW(worig.c_str());
    if (orig_attrs == INVALID_FILE_ATTRIBUTES || (orig_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (err) *err = "目标目录不存在或无法访问";
        return false;
    }
    entry.win_orig_attrs = static_cast<uint32_t>(orig_attrs);

    std::string dlock_path = entry.path + ".dlock";
    std::string vault_path;
    if (choose_same_volume_vault(entry.path, state_dir, entry.id, vault_path)) {
        std::string vault_root = parent_of_win_path(vault_path);
        std::wstring wvault = utf8_to_wide(vault_path);
        if (MoveFileW(worig.c_str(), wvault.c_str())) {
            SetFileAttributesW(wvault.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
            win_lock_directory(vault_path, entry.win_sid, nullptr);
            seal_vault_root_if_nonempty(vault_root, false);

            if (!create_locked_dlock_stub(dlock_path, entry)) {
                ensure_hidden_dir_unlocked(vault_root);
                win_unlock_directory(vault_path, entry.win_sid, nullptr);
                SetFileAttributesW(wvault.c_str(), orig_attrs);
                MoveFileW(wvault.c_str(), worig.c_str());
                seal_vault_root_if_nonempty(vault_root, true);
                if (err) *err = "创建加密目录入口文件失败";
                return false;
            }
            // Remove legacy .lnk if any existed
            DeleteFileW(utf8_to_wide(entry.path + ".lnk").c_str());

            entry.vault_path = vault_path;
            entry.in_place_acl = false;
            entry.locked = true;
            notify_shell_parent_updated(entry.path);
            return true;
        }
        DWORD move_err = GetLastError();
        seal_vault_root_if_nonempty(vault_root, true);
        if (!allow_in_place_fallback) {
            if (move_err == ERROR_SHARING_VIOLATION || move_err == ERROR_ACCESS_DENIED) {
                if (err) *err = "目录或其内部文件正在被其他程序或窗口占用，请先关闭后再锁定";
            } else {
                if (err) *err = "锁定目录失败 (Win32 错误码: " + std::to_string(move_err) + ")";
            }
            return false;
        }
    } else if (!allow_in_place_fallback) {
        if (err) *err = "无法为该路径创建本地保护区位置";
        return false;
    }

    // Fallback: in-place ACL lock (used for UNC paths or when open handle prevents moving during Ctrl+Alt+L)
    if (!win_lock_directory(entry.path, entry.win_sid, err)) {
        return false;
    }
    entry.vault_path.clear();
    entry.in_place_acl = true;
    entry.locked = true;
    notify_shell_parent_updated(entry.path);
    return true;
}

bool win_smart_unlock(DirEntry& entry, std::string* err) {
    if (!entry.vault_path.empty()) {
        std::wstring worig = utf8_to_wide(entry.path);
        std::wstring wvault = utf8_to_wide(entry.vault_path);

        if (GetFileAttributesW(worig.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (err) *err = "原路径已存在同名文件或目录，请先移走或重命名后再解锁（原数据在保护区中完好无损）";
            return false;
        }

        std::string vault_root = parent_of_win_path(entry.vault_path);
        ensure_hidden_dir_unlocked(vault_root);

        DWORD vault_attr = GetFileAttributesW(wvault.c_str());
        if (vault_attr == INVALID_FILE_ATTRIBUTES) {
            seal_vault_root_if_nonempty(vault_root, false);
            if (err) *err = "未在数据保护区中找到已锁定的目录";
            return false;
        }

        win_unlock_directory(entry.vault_path, entry.win_sid, nullptr);
        DWORD restore_attrs = entry.win_orig_attrs ? static_cast<DWORD>(entry.win_orig_attrs)
                                                   : FILE_ATTRIBUTE_DIRECTORY;
        SetFileAttributesW(wvault.c_str(), restore_attrs & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));

        BOOL moved = FALSE;
        DWORD last_err = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (MoveFileW(wvault.c_str(), worig.c_str())) {
                moved = TRUE;
                break;
            }
            last_err = GetLastError();
            Sleep(80);
        }
        if (!moved) {
            SetFileAttributesW(wvault.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
            win_lock_directory(entry.vault_path, entry.win_sid, nullptr);
            seal_vault_root_if_nonempty(vault_root, false);
            if (err) *err = "还原目录到原路径失败 (Win32 错误码: " + std::to_string(last_err) + ")";
            return false;
        }

        SetFileAttributesW(worig.c_str(), restore_attrs);
        delete_protected_file(entry.path + ".dlock");
        DeleteFileW(utf8_to_wide(entry.path + ".lnk").c_str());
        seal_vault_root_if_nonempty(vault_root, true);
        notify_shell_parent_updated(entry.path);

        entry.vault_path.clear();
        entry.in_place_acl = false;
        entry.locked = false;
        entry.last_unlocked_at = StateStore::now_epoch();
        return true;
    }

    // Legacy / in-place ACL unlock
    if (!win_unlock_directory(entry.path, entry.win_sid, err)) {
        return false;
    }
    entry.in_place_acl = false;
    entry.locked = false;
    entry.last_unlocked_at = StateStore::now_epoch();
    notify_shell_parent_updated(entry.path);
    return true;
}

}  // namespace dirlocker
