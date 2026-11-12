#include "acl_locker.hpp"

#include "../core/json_mini.hpp"
#include "../core/path_guard.hpp"
#include "../core/state_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <exdisp.h>
#include <oleauto.h>
#include <restartmanager.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>
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

std::string trim_trailing_win_slashes(std::string p) {
    for (char& ch : p) {
        if (ch == '/') ch = '\\';
    }
    while (p.size() > 3 && p.back() == '\\') {
        p.pop_back();
    }
    if (p.size() > 2 && p[1] != ':' && p.back() == '\\') {
        p.pop_back();
    }
    return p;
}

std::string normalize_win_key(const std::string& p) {
    std::string s = trim_trailing_win_slashes(p);
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

bool is_same_or_subpath_win(const std::string& candidate, const std::string& base_dir) {
    std::string c = normalize_win_key(candidate);
    std::string b = normalize_win_key(base_dir);
    if (c.empty() || b.empty()) return false;
    if (c == b) return true;
    if (c.size() > b.size() && c.compare(0, b.size(), b) == 0) {
        return b.back() == '\\' || c[b.size()] == '\\';
    }
    return false;
}

bool is_unc_or_nondrive_win(const std::string& path) {
    return path.size() < 3 || path[1] != ':';
}

bool is_win_drive_ready(const std::string& path) {
    if (path.size() < 2 || path[1] != ':') return true;
    char drv = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    if (drv < 'A' || drv > 'Z') return true;
    wchar_t root[4] = {static_cast<wchar_t>(drv), L':', L'\\', L'\0'};
    return GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES;
}

bool dir_exists_win(const std::string& p) {
    if (p.empty()) return false;
    DWORD attrs = GetFileAttributesW(utf8_to_wide(p).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool file_exists_win(const std::string& p) {
    if (p.empty()) return false;
    DWORD attrs = GetFileAttributesW(utf8_to_wide(p).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string percent_decode_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_nibble(s[i + 1]);
            int lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string decode_file_url_to_win_path(const wchar_t* wurl) {
    if (!wurl || !*wurl) return {};
    std::string raw = wide_to_utf8(wurl);
    if (raw.size() < 7) return {};
    std::string prefix = raw.substr(0, 7);
    for (char& c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (prefix != "file://") return {};

    std::string rest = raw.substr(7);
    std::string path;
    if (!rest.empty() && rest[0] == '/') {
        // file:///C:/foo -> C:\foo
        path = percent_decode_utf8(rest.substr(1));
    } else {
        // file://server/share -> \\server\share
        path = "\\\\" + percent_decode_utf8(rest);
    }
    return trim_trailing_win_slashes(path);
}

void push_unique_proc(std::vector<std::string>& out,
                      std::unordered_set<std::string>& seen,
                      const std::string& label,
                      size_t max_items = 6) {
    if (label.empty() || out.size() >= max_items) return;
    std::string key = label;
    for (char& ch : key) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (seen.insert(key).second) {
        out.push_back(label);
    }
}

void collect_explorer_blocking_windows(const std::string& target_path,
                                       std::vector<std::string>& out,
                                       std::unordered_set<std::string>& seen) {
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool should_uninit = (hr_com == S_OK || hr_com == S_FALSE);

    IShellWindows* psw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, reinterpret_cast<void**>(&psw));
    if (SUCCEEDED(hr) && psw) {
        long count = 0;
        if (SUCCEEDED(psw->get_Count(&count))) {
            for (long i = 0; i < count; ++i) {
                VARIANT idx;
                VariantInit(&idx);
                idx.vt = VT_I4;
                idx.lVal = i;
                IDispatch* disp = nullptr;
                if (SUCCEEDED(psw->Item(idx, &disp)) && disp) {
                    IWebBrowserApp* web = nullptr;
                    if (SUCCEEDED(disp->QueryInterface(IID_IWebBrowserApp,
                                                       reinterpret_cast<void**>(&web))) &&
                        web) {
                        BSTR url = nullptr;
                        if (SUCCEEDED(web->get_LocationURL(&url)) && url) {
                            std::string win_loc = decode_file_url_to_win_path(url);
                            SysFreeString(url);
                            if (!win_loc.empty() && is_same_or_subpath_win(win_loc, target_path)) {
                                push_unique_proc(out, seen, "explorer.exe (文件资源管理器窗口)");
                            }
                        }
                        web->Release();
                    }
                    disp->Release();
                }
                VariantClear(&idx);
            }
        }
        psw->Release();
    }

    if (should_uninit) {
        CoUninitialize();
    }
}

void collect_rm_candidate_files(const std::string& target_path,
                                  std::vector<std::wstring>& busy_files,
                                  std::vector<std::wstring>& sample_files) {
    struct Node {
        std::wstring dir;
        int depth;
    };
    std::vector<Node> stack;
    stack.push_back({utf8_to_wide(trim_trailing_win_slashes(target_path)), 0});

    constexpr int kMaxDepth = 8;
    constexpr size_t kMaxScanned = 512;
    constexpr size_t kMaxBusy = 64;
    constexpr size_t kMaxSamples = 24;
    size_t scanned = 0;

    while (!stack.empty() && scanned < kMaxScanned && busy_files.size() < kMaxBusy) {
        Node cur = std::move(stack.back());
        stack.pop_back();

        std::wstring pat = cur.dir;
        if (!pat.empty() && pat.back() != L'\\') pat.push_back(L'\\');
        pat.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            std::wstring full = cur.dir;
            if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
            full += fd.cFileName;

            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (cur.depth < kMaxDepth) {
                    stack.push_back({std::move(full), cur.depth + 1});
                }
                continue;
            }

            ++scanned;
            HANDLE fh = CreateFileW(full.c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fh != INVALID_HANDLE_VALUE) {
                CloseHandle(fh);
                if (sample_files.size() < kMaxSamples) {
                    sample_files.push_back(std::move(full));
                }
            } else if (GetLastError() == ERROR_SHARING_VIOLATION) {
                busy_files.push_back(std::move(full));
            }
            if (scanned >= kMaxScanned || busy_files.size() >= kMaxBusy) {
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

std::string query_proc_exe_basename(DWORD pid) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    std::string out;
    if (QueryFullProcessImageNameW(hp, 0, buf, &len) && len > 0) {
        std::string full = wide_to_utf8(buf);
        size_t sep = full.find_last_of("\\/");
        out = (sep == std::string::npos) ? full : full.substr(sep + 1);
    }
    CloseHandle(hp);
    return out;
}

void collect_rm_blocking_processes(const std::string& target_path,
                                   std::vector<std::string>& out,
                                   std::unordered_set<std::string>& seen) {
    HMODULE rm = LoadLibraryW(L"rstrtmgr.dll");
    if (!rm) return;

    auto load_fn = [rm](const char* name, auto tag) {
        using Fn = decltype(tag);
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(rm, name)));
    };
    auto fn_start = load_fn("RmStartSession", static_cast<decltype(&RmStartSession)>(nullptr));
    auto fn_reg = load_fn("RmRegisterResources", static_cast<decltype(&RmRegisterResources)>(nullptr));
    auto fn_list = load_fn("RmGetList", static_cast<decltype(&RmGetList)>(nullptr));
    auto fn_end = load_fn("RmEndSession", static_cast<decltype(&RmEndSession)>(nullptr));
    if (!fn_start || !fn_reg || !fn_list || !fn_end) {
        FreeLibrary(rm);
        return;
    }

    std::vector<std::wstring> busy_files;
    std::vector<std::wstring> sample_files;
    collect_rm_candidate_files(target_path, busy_files, sample_files);
    const std::vector<std::wstring>& candidates = busy_files.empty() ? sample_files : busy_files;
    if (candidates.empty()) {
        FreeLibrary(rm);
        return;
    }

    DWORD sess = 0;
    WCHAR sess_key[CCH_RM_SESSION_KEY + 1] = {};
    if (fn_start(&sess, 0, sess_key) != ERROR_SUCCESS) {
        FreeLibrary(rm);
        return;
    }

    std::vector<LPCWSTR> ptrs;
    ptrs.reserve(candidates.size());
    for (const auto& w : candidates) {
        ptrs.push_back(w.c_str());
    }

    if (fn_reg(sess, static_cast<UINT>(ptrs.size()), ptrs.data(), 0, nullptr, 0, nullptr) ==
        ERROR_SUCCESS) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reboot = RmRebootReasonNone;
        std::vector<RM_PROCESS_INFO> infos(16);
        count = static_cast<UINT>(infos.size());
        DWORD rc = fn_list(sess, &needed, &count, infos.data(), &reboot);
        for (int attempt = 0; rc == ERROR_MORE_DATA && attempt < 3; ++attempt) {
            infos.resize(needed > 0 ? needed : 32);
            count = static_cast<UINT>(infos.size());
            rc = fn_list(sess, &needed, &count, infos.data(), &reboot);
        }
        if (rc == ERROR_SUCCESS) {
            DWORD self_pid = GetCurrentProcessId();
            for (UINT i = 0; i < count; ++i) {
                DWORD pid = infos[i].Process.dwProcessId;
                if (pid == 0 || pid == self_pid) continue;

                std::string exe = query_proc_exe_basename(pid);
                std::string app = wide_to_utf8(infos[i].strAppName);
                std::string label;
                if (!exe.empty() && !app.empty()) {
                    std::string exe_low = exe;
                    std::string app_low = app;
                    for (char& c : exe_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (char& c : app_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (exe_low != app_low && exe_low != app_low + ".exe") {
                        label = exe + " (" + app + ")";
                    } else {
                        label = exe;
                    }
                } else {
                    label = !exe.empty() ? exe : app;
                }
                push_unique_proc(out, seen, label);
            }
        }
    }

    fn_end(sess);
    FreeLibrary(rm);
}

std::string format_occupancy_error(const std::vector<std::string>& procs, DWORD win32_err) {
    if (!procs.empty()) {
        std::string joined;
        for (size_t i = 0; i < procs.size(); ++i) {
            if (i > 0) joined += "、";
            joined += procs[i];
        }
        return "目录或其中文件正被以下程序占用，无法上锁：" + joined +
               "\n请先关闭相关文档或窗口后再试（注：记事本等打开后不持续占用的程序不影响上锁）。";
    }
    if (win32_err == ERROR_SHARING_VIOLATION || win32_err == ERROR_ACCESS_DENIED) {
        return "目录或其中文件正被其他程序占用（例如命令行工作目录停留在其中，或资源管理器预览窗格打开了文件），"
               "请先关闭相关程序或窗口后再试。";
    }
    return "锁定目录失败 (Win32 错误码: " + std::to_string(win32_err) + ")";
}

std::vector<std::string> collect_existing_vault_roots(const std::string& state_dir) {
    std::vector<std::string> roots;
    std::unordered_set<std::string> seen;
    auto maybe_add = [&](const std::string& candidate) {
        if (candidate.empty()) return;
        std::string key = normalize_win_key(candidate);
        if (seen.insert(key).second && dir_exists_win(candidate)) {
            roots.push_back(candidate);
        }
    };

    if (!state_dir.empty()) {
        maybe_add(trim_trailing_win_slashes(state_dir) + "\\Vault");
    }
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        char drv = static_cast<char>('A' + i);
        wchar_t wroot[4] = {static_cast<wchar_t>(drv), L':', L'\\', L'\0'};
        UINT dtype = GetDriveTypeW(wroot);
        if (dtype != DRIVE_FIXED && dtype != DRIVE_REMOVABLE) continue;
        std::string vroot = std::string(1, drv) + ":\\$目录密匣_重要数据保护区_切勿删除";
        maybe_add(vroot);
    }
    return roots;
}

std::vector<std::string> list_vault_subdirs_readonly(const std::string& vroot) {
    std::vector<std::string> out;
    if (!dir_exists_win(vroot)) return out;

    // Vault root has deny-list ACL while non-empty; temporarily lift just for directory listing then re-seal.
    bool had_acl = win_is_directory_locked(vroot, "S-1-1-0");
    if (had_acl) {
        remove_deny_ace(vroot, kLockDenyMask, "S-1-1-0", nullptr);
    }

    std::wstring pat = utf8_to_wide(trim_trailing_win_slashes(vroot)) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                out.push_back(trim_trailing_win_slashes(vroot) + "\\" + wide_to_utf8(fd.cFileName));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (had_acl) {
        seal_vault_root_if_nonempty(vroot, false);
    }
    return out;
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

std::vector<std::string> win_detect_blocking_processes(const std::string& target_path) {
    std::vector<std::string> procs;
    std::unordered_set<std::string> seen;
    if (!dir_exists_win(target_path)) {
        return procs;
    }
    collect_rm_blocking_processes(target_path, procs, seen);
    collect_explorer_blocking_windows(target_path, procs, seen);
    return procs;
}

std::string win_make_companion_dlock_path(const std::string& dir_path) {
    std::string clean = trim_trailing_win_slashes(dir_path);
    return clean + "（已锁定，点此解锁）.dlock";
}

bool win_repair_locked_entry_stub(DirEntry& entry) {
    if (!entry.locked) return false;

    if (entry.in_place_acl) {
        if (!dir_exists_win(entry.path)) return false;
        if (!win_is_directory_locked(entry.path, entry.win_sid)) {
            win_lock_directory(entry.path, entry.win_sid, nullptr);
        }
        std::string stub = entry.stub_path.empty()
            ? win_make_companion_dlock_path(entry.path)
            : entry.stub_path;
        if (file_exists_win(stub)) {
            entry.stub_path = stub;
            return false;
        }
        if (!create_locked_dlock_stub(stub, entry)) return false;
        entry.stub_path = stub;
        notify_shell_parent_updated(stub);
        return true;
    }

    if (entry.vault_path.empty()) return false;
    if (!dir_exists_win(entry.vault_path)) return false;
    if (GetFileAttributesW(utf8_to_wide(entry.path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    std::string dlock_path = entry.path + ".dlock";
    if (GetFileAttributesW(utf8_to_wide(dlock_path).c_str()) != INVALID_FILE_ATTRIBUTES) {
        entry.stub_path = dlock_path;
        return false;  // already present
    }

    if (!create_locked_dlock_stub(dlock_path, entry)) return false;
    entry.stub_path = dlock_path;

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

    // Strict occupancy check upfront when fallback is disabled (covers Word/WPS/PDF/Explorer etc.,
    // while allowing Notepad-style editors that close handles immediately after loading).
    if (!allow_in_place_fallback) {
        std::vector<std::string> procs = win_detect_blocking_processes(entry.path);
        if (!procs.empty()) {
            if (err) *err = format_occupancy_error(procs, ERROR_SHARING_VIOLATION);
            return false;
        }
    }

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
            // Remove legacy .lnk or stale companion stub if any existed
            DeleteFileW(utf8_to_wide(entry.path + ".lnk").c_str());
            delete_protected_file(win_make_companion_dlock_path(entry.path));

            entry.vault_path = vault_path;
            entry.stub_path = dlock_path;
            entry.in_place_acl = false;
            entry.locked = true;
            notify_shell_parent_updated(entry.path);
            return true;
        }
        DWORD move_err = GetLastError();
        seal_vault_root_if_nonempty(vault_root, true);
        if (!allow_in_place_fallback) {
            std::vector<std::string> procs = win_detect_blocking_processes(entry.path);
            if (err) *err = format_occupancy_error(procs, move_err);
            return false;
        }
    } else if (!allow_in_place_fallback &&
               !is_unc_or_nondrive_win(entry.path)) {
        if (err) *err = "无法为该路径创建本地保护区位置";
        return false;
    }

    // In-place ACL lock (UNC unoccupied shares or explicit user fallback when occupied)
    if (!win_lock_directory(entry.path, entry.win_sid, err)) {
        return false;
    }
    std::string companion = win_make_companion_dlock_path(entry.path);
    if (create_locked_dlock_stub(companion, entry)) {
        entry.stub_path = companion;
    } else {
        entry.stub_path.clear();
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
        if (!entry.stub_path.empty() && entry.stub_path != entry.path + ".dlock") {
            delete_protected_file(entry.stub_path);
        }
        delete_protected_file(win_make_companion_dlock_path(entry.path));
        DeleteFileW(utf8_to_wide(entry.path + ".lnk").c_str());
        seal_vault_root_if_nonempty(vault_root, true);
        notify_shell_parent_updated(entry.path);

        entry.vault_path.clear();
        entry.stub_path.clear();
        entry.in_place_acl = false;
        entry.locked = false;
        entry.last_unlocked_at = StateStore::now_epoch();
        return true;
    }

    // In-place ACL unlock (and clean up companion .dlock stub)
    if (!win_unlock_directory(entry.path, entry.win_sid, err)) {
        return false;
    }
    if (!entry.stub_path.empty()) {
        delete_protected_file(entry.stub_path);
        notify_shell_parent_updated(entry.stub_path);
    }
    delete_protected_file(win_make_companion_dlock_path(entry.path));
    delete_protected_file(entry.path + ".dlock");

    entry.stub_path.clear();
    entry.in_place_acl = false;
    entry.locked = false;
    entry.last_unlocked_at = StateStore::now_epoch();
    notify_shell_parent_updated(entry.path);
    return true;
}

bool win_try_upgrade_in_place_entry(DirEntry& entry, const std::string&,
                                    const std::string& state_dir, bool* upgraded,
                                    std::string* err) {
    if (upgraded) *upgraded = false;
    if (!entry.locked || !entry.in_place_acl || is_unc_or_nondrive_win(entry.path)) {
        return true;
    }
    if (!is_win_drive_ready(entry.path) || !dir_exists_win(entry.path)) {
        return false;
    }
    if (!win_detect_blocking_processes(entry.path).empty()) {
        return true;
    }

    std::string vault_path;
    if (!choose_same_volume_vault(entry.path, state_dir, entry.id, vault_path)) {
        return false;
    }

    std::wstring worig = utf8_to_wide(entry.path);
    DWORD orig_attrs = GetFileAttributesW(worig.c_str());
    if (orig_attrs != INVALID_FILE_ATTRIBUTES) {
        entry.win_orig_attrs = static_cast<uint32_t>(orig_attrs);
    }

    std::string vault_root = parent_of_win_path(vault_path);
    std::wstring wvault = utf8_to_wide(vault_path);

    // Briefly lift our own in-place ACL solely to test atomic MoveFileW into Vault.
    win_unlock_directory(entry.path, entry.win_sid, nullptr);
    if (!MoveFileW(worig.c_str(), wvault.c_str())) {
        // Still held by e.g. cmd.exe CWD -> restore in-place ACL immediately.
        win_lock_directory(entry.path, entry.win_sid, nullptr);
        seal_vault_root_if_nonempty(vault_root, true);
        return true;
    }

    SetFileAttributesW(wvault.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    win_lock_directory(vault_path, entry.win_sid, nullptr);
    seal_vault_root_if_nonempty(vault_root, false);

    std::string std_stub = entry.path + ".dlock";
    if (!create_locked_dlock_stub(std_stub, entry)) {
        ensure_hidden_dir_unlocked(vault_root);
        win_unlock_directory(vault_path, entry.win_sid, nullptr);
        SetFileAttributesW(wvault.c_str(), entry.win_orig_attrs ? entry.win_orig_attrs : FILE_ATTRIBUTE_DIRECTORY);
        MoveFileW(wvault.c_str(), worig.c_str());
        win_lock_directory(entry.path, entry.win_sid, nullptr);
        seal_vault_root_if_nonempty(vault_root, true);
        if (err) *err = "迁移到保护区时创建 .dlock 入口失败";
        return false;
    }

    std::string old_companion = entry.stub_path;
    if (!old_companion.empty() && normalize_win_key(old_companion) != normalize_win_key(std_stub)) {
        delete_protected_file(old_companion);
    }
    delete_protected_file(win_make_companion_dlock_path(entry.path));

    entry.vault_path = vault_path;
    entry.stub_path = std_stub;
    entry.in_place_acl = false;
    entry.locked = true;
    notify_shell_parent_updated(entry.path);
    if (upgraded) *upgraded = true;
    return true;
}

WinHealthReport win_diagnose_state(const StateStore& store, const std::string& state_dir) {
    WinHealthReport rep;
    std::unordered_set<std::string> known_vaults;

    for (const auto& entry : store.entries()) {
        ++rep.total_entries;
        if (!entry.vault_path.empty()) {
            known_vaults.insert(normalize_win_key(entry.vault_path));
        }

        WinHealthItem item;
        item.entry_id = entry.id;
        item.display_path = entry.path;
        item.vault_path = entry.vault_path;

        if (!is_win_drive_ready(entry.path) ||
            (!entry.vault_path.empty() && !is_win_drive_ready(entry.vault_path))) {
            item.kind = WinHealthItem::IssueKind::kUnmountedDrive;
            item.summary = "所在磁盘或移动驱动器当前未就绪";
            item.repair_hint = "若为U盘或移动硬盘，请接入后再扫描";
            ++rep.unmounted_count;
            rep.items.push_back(std::move(item));
            continue;
        }

        bool orig_exists = dir_exists_win(entry.path);
        bool vault_exists = !entry.vault_path.empty() && dir_exists_win(entry.vault_path);

        if (entry.locked && !entry.in_place_acl) {
            if (orig_exists && vault_exists) {
                item.kind = WinHealthItem::IssueKind::kLeftoverVaultAndDir;
                item.summary = "保护区与原路径同时存在同名目录（冲突）";
                item.repair_hint = "为保数据安全不会自动合并，请打开设置/保护区手工核对";
                ++rep.manual_count;
                rep.items.push_back(std::move(item));
                continue;
            }
            if (vault_exists && !orig_exists) {
                std::string stub = entry.path + ".dlock";
                if (!file_exists_win(stub)) {
                    item.kind = WinHealthItem::IssueKind::kMissingDlockStub;
                    item.summary = "保护区数据完好，但 .dlock 入口文件丢失";
                    item.repair_hint = "可一键补建 .dlock 入口并校验保护区权限";
                    item.auto_repairable = true;
                    ++rep.repairable_count;
                    rep.items.push_back(std::move(item));
                } else {
                    ++rep.ok_count;
                }
                continue;
            }
            if (!vault_exists && orig_exists) {
                if (!win_is_directory_locked(entry.path, entry.win_sid)) {
                    item.kind = WinHealthItem::IssueKind::kExternallyRestored;
                    item.summary = "目录已在外部恢复为未锁定状态，记录仍标记已锁定";
                    item.repair_hint = "可一键同步状态为“未锁定”并清理残留入口";
                    item.auto_repairable = true;
                    ++rep.repairable_count;
                } else {
                    item.kind = WinHealthItem::IssueKind::kMissingCompanionStub;
                    item.summary = "目录处于原位锁定状态，需要同步记录与伴随入口";
                    item.repair_hint = "可一键补齐原位伴随 .dlock 入口";
                    item.auto_repairable = true;
                    ++rep.repairable_count;
                }
                rep.items.push_back(std::move(item));
                continue;
            }
            item.kind = WinHealthItem::IssueKind::kVaultMissing;
            item.summary = "原目录与保护区目录均不存在（可能被外部卸载或删除）";
            item.repair_hint = "核实数据不在磁盘后，可勾选“清理已失联记录”移出列表";
            item.allow_remove_record = true;
            ++rep.manual_count;
            rep.items.push_back(std::move(item));
            continue;
        }

        if (entry.locked && entry.in_place_acl) {
            if (!orig_exists) {
                item.kind = WinHealthItem::IssueKind::kVaultMissing;
                item.summary = "原位锁定目录已不存在于磁盘";
                item.repair_hint = "核实磁盘状态后可勾选“清理已失联记录”移出列表";
                item.allow_remove_record = true;
                ++rep.manual_count;
                rep.items.push_back(std::move(item));
                continue;
            }
            if (!win_is_directory_locked(entry.path, entry.win_sid)) {
                item.kind = WinHealthItem::IssueKind::kExternallyRestored;
                item.summary = "原位锁权限已被外部解除，但记录仍标记已锁定";
                item.repair_hint = "可一键同步为“未锁定”并清理伴随 .dlock";
                item.auto_repairable = true;
                ++rep.repairable_count;
                rep.items.push_back(std::move(item));
                continue;
            }
            std::string stub = entry.stub_path.empty()
                ? win_make_companion_dlock_path(entry.path)
                : entry.stub_path;
            if (!file_exists_win(stub)) {
                item.kind = WinHealthItem::IssueKind::kMissingCompanionStub;
                item.summary = "原位锁定目录正常，但伴随（点此解锁）.dlock 丢失";
                item.repair_hint = "可一键补建伴随 .dlock 解锁入口";
                item.auto_repairable = true;
                ++rep.repairable_count;
                rep.items.push_back(std::move(item));
            } else {
                ++rep.ok_count;
            }
            continue;
        }

        // Unlocked entry checks
        bool has_stale_stub = file_exists_win(entry.path + ".dlock") ||
                              file_exists_win(win_make_companion_dlock_path(entry.path)) ||
                              (!entry.stub_path.empty() && file_exists_win(entry.stub_path));
        if (has_stale_stub) {
            item.kind = WinHealthItem::IssueKind::kStaleUnlockedWithStub;
            item.summary = "目录已解锁，但旁边仍残留多余的 .dlock 入口文件";
            item.repair_hint = "可一键清理残留 .dlock 文件";
            item.auto_repairable = true;
            ++rep.repairable_count;
            rep.items.push_back(std::move(item));
            continue;
        }
        if (!orig_exists) {
            item.kind = WinHealthItem::IssueKind::kMissingOrigDirUnlocked;
            item.summary = "未锁定目录已被外部移动或删除";
            item.repair_hint = "若已不再需要该目录，可勾选“清理已失联记录”移出列表";
            item.allow_remove_record = true;
            ++rep.manual_count;
            rep.items.push_back(std::move(item));
            continue;
        }
        ++rep.ok_count;
    }

    for (const std::string& vroot : collect_existing_vault_roots(state_dir)) {
        for (const std::string& sub : list_vault_subdirs_readonly(vroot)) {
            if (known_vaults.count(normalize_win_key(sub)) == 0) {
                WinHealthItem orphan;
                orphan.display_path = sub;
                orphan.vault_path = sub;
                orphan.kind = WinHealthItem::IssueKind::kOrphanVaultDir;
                orphan.summary = "保护区中存在未绑定状态记录的目录（数据完好）";
                orphan.repair_hint = "系统不会自动删除保护区数据，请按《应急恢复说明》手工核对";
                ++rep.orphan_vault_count;
                ++rep.manual_count;
                rep.items.push_back(std::move(orphan));
            }
        }
    }
    return rep;
}

bool win_repair_state(StateStore& store, const std::string& state_dir, bool clean_dead_records,
                      int* repaired_count, std::vector<std::string>* details,
                      std::string* err) {
    if (repaired_count) *repaired_count = 0;
    if (details) details->clear();

    WinHealthReport rep = win_diagnose_state(store, state_dir);
    bool changed = false;
    int count = 0;
    std::unordered_set<std::string> ids_to_remove;

    for (const auto& item : rep.items) {
        DirEntry* entry = item.entry_id.empty() ? nullptr : store.find_by_id(item.entry_id);
        if (!entry) continue;

        switch (item.kind) {
            case WinHealthItem::IssueKind::kMissingDlockStub: {
                if (win_repair_locked_entry_stub(*entry)) {
                    win_lock_directory(entry->vault_path, entry->win_sid, nullptr);
                    changed = true;
                    ++count;
                    if (details) details->push_back("已补建 .dlock 入口: " + entry->path);
                }
                break;
            }
            case WinHealthItem::IssueKind::kMissingCompanionStub: {
                if (!entry->in_place_acl && entry->vault_path.empty()) {
                    entry->in_place_acl = true;
                } else if (!entry->in_place_acl && !dir_exists_win(entry->vault_path)) {
                    entry->in_place_acl = true;
                    entry->vault_path.clear();
                }
                if (win_repair_locked_entry_stub(*entry)) {
                    changed = true;
                    ++count;
                    if (details) details->push_back("已补建伴随解锁入口: " + entry->path);
                }
                break;
            }
            case WinHealthItem::IssueKind::kExternallyRestored: {
                delete_protected_file(entry->path + ".dlock");
                if (!entry->stub_path.empty()) delete_protected_file(entry->stub_path);
                delete_protected_file(win_make_companion_dlock_path(entry->path));
                entry->locked = false;
                entry->in_place_acl = false;
                entry->vault_path.clear();
                entry->stub_path.clear();
                notify_shell_parent_updated(entry->path);
                changed = true;
                ++count;
                if (details) details->push_back("已同步外部恢复状态为未锁定: " + entry->path);
                break;
            }
            case WinHealthItem::IssueKind::kStaleUnlockedWithStub: {
                delete_protected_file(entry->path + ".dlock");
                if (!entry->stub_path.empty()) delete_protected_file(entry->stub_path);
                delete_protected_file(win_make_companion_dlock_path(entry->path));
                entry->stub_path.clear();
                notify_shell_parent_updated(entry->path);
                changed = true;
                ++count;
                if (details) details->push_back("已清理残留 .dlock 文件: " + entry->path);
                break;
            }
            case WinHealthItem::IssueKind::kVaultMissing:
            case WinHealthItem::IssueKind::kMissingOrigDirUnlocked: {
                if (clean_dead_records) {
                    delete_protected_file(entry->path + ".dlock");
                    if (!entry->stub_path.empty()) delete_protected_file(entry->stub_path);
                    delete_protected_file(win_make_companion_dlock_path(entry->path));
                    ids_to_remove.insert(entry->id);
                    changed = true;
                    ++count;
                    if (details) details->push_back("已移除失联记录: " + entry->path);
                }
                break;
            }
            default:
                break;
        }
    }

    if (!ids_to_remove.empty()) {
        auto& vec = store.entries_mut();
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [&](const DirEntry& e) { return ids_to_remove.count(e.id) > 0; }),
                  vec.end());
    }

    if (changed) {
        if (!store.save(err)) {
            return false;
        }
    }
    if (repaired_count) *repaired_count = count;
    return true;
}

}  // namespace dirlocker
