#include "path_guard.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <climits>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dirlocker {

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], len);
    return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}
#endif

namespace {

#ifdef _WIN32
std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string get_env_utf8(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring buf(needed, L'\0');
    DWORD got = GetEnvironmentVariableW(name, &buf[0], needed);
    if (got == 0 || got >= needed) return {};
    buf.resize(got);
    return wide_to_utf8(buf);
}

std::string get_windows_dir_utf8() {
    wchar_t buf[MAX_PATH] = {};
    UINT len = GetWindowsDirectoryW(buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "C:\\Windows";
    return wide_to_utf8(std::wstring(buf, len));
}

bool is_drive_root_win(const std::string& p) {
    return p.size() == 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && p[2] == '\\';
}

bool is_unc_root_win(const std::string& p) {
    if (p.size() < 5 || p[0] != '\\' || p[1] != '\\') return false;
    size_t server_end = p.find('\\', 2);
    if (server_end == std::string::npos || server_end == 2) return false;
    size_t share_end = p.find('\\', server_end + 1);
    return share_end == std::string::npos;
}
#else
std::string detect_user_home() {
    const char* env_home = std::getenv("HOME");
    if (env_home && *env_home) return env_home;
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return {};
}
#endif

}  // namespace

std::string normalize_path_lexical(const std::string& path) {
    if (path.empty()) return {};
    std::string s = path;
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
    if (s.size() >= 4 && s.compare(0, 4, "\\\\?\\") == 0) {
        if (s.size() >= 8 && s.compare(0, 8, "\\\\?\\UNC\\") == 0) {
            s = "\\\\" + s.substr(8);
        } else {
            s = s.substr(4);
        }
    }
    // Collapse duplicate backslashes except leading UNC "\\"
    std::string collapsed;
    collapsed.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i > 0 && s[i - 1] == '\\') {
            if (i == 1) collapsed.push_back('\\');
            continue;
        }
        collapsed.push_back(s[i]);
    }
    s = std::move(collapsed);
    while (s.size() > 3 && s.back() == '\\') {
        s.pop_back();
    }
    if (s.size() == 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        s.push_back('\\');
    }
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    }
#else
    std::string collapsed;
    collapsed.reserve(s.size());
    for (char c : s) {
        if (c == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
        collapsed.push_back(c);
    }
    s = std::move(collapsed);
    while (s.size() > 2 && s.size() >= 2 && s.compare(s.size() - 2, 2, "/.") == 0) {
        s.erase(s.size() - 2);
    }
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#endif
    return s;
}

bool paths_equal(const std::string& a, const std::string& b) {
    std::string na = normalize_path_lexical(a);
    std::string nb = normalize_path_lexical(b);
#ifdef _WIN32
    return ascii_lower(na) == ascii_lower(nb);
#else
    return na == nb;
#endif
}

bool is_same_or_subpath(const std::string& child, const std::string& parent) {
    std::string c = normalize_path_lexical(child);
    std::string p = normalize_path_lexical(parent);
    if (c.empty() || p.empty()) return false;
#ifdef _WIN32
    c = ascii_lower(c);
    p = ascii_lower(p);
    if (c == p) return true;
    if (p.back() == '\\') {
        return c.size() > p.size() && c.compare(0, p.size(), p) == 0;
    }
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '\\';
#else
    if (c == p) return true;
    if (p == "/") return !c.empty() && c[0] == '/';
    return c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/';
#endif
}

bool paths_overlap(const std::string& a, const std::string& b) {
    return is_same_or_subpath(a, b) || is_same_or_subpath(b, a);
}

bool is_protected_system_path(const std::string& canonical_path,
                              const std::string& user_home,
                              const std::vector<std::string>& extra_protected,
                              std::string* reason) {
    std::string norm = normalize_path_lexical(canonical_path);
    auto reject = [&](const std::string& msg) {
        if (reason) *reason = msg;
        return true;
    };

    if (norm.empty()) return reject("路径为空");

#ifdef _WIN32
    if (is_drive_root_win(norm) || is_unc_root_win(norm)) {
        return reject("禁止锁定磁盘根目录或网络共享根目录");
    }
    if (norm.size() >= 3 && norm[1] == ':' && norm[2] == '\\') {
        std::string drv_vault_legacy = norm.substr(0, 3) + "$DirShieldVault";
        std::string drv_vault = norm.substr(0, 3) + "$目录密匣_重要数据保护区_切勿删除";
        if (is_same_or_subpath(norm, drv_vault) || is_same_or_subpath(norm, drv_vault_legacy)) {
            return reject("禁止锁定目录密匣系统数据保护区");
        }
    }

    std::string win_dir = normalize_path_lexical(get_windows_dir_utf8());
    std::string sys_drive = win_dir.size() >= 3 ? win_dir.substr(0, 3) : "C:\\";

    std::vector<std::string> prefix_blocked = {
        win_dir,
        sys_drive + "Program Files",
        sys_drive + "Program Files (x86)",
        sys_drive + "ProgramData",
        sys_drive + "Recovery",
        sys_drive + "$Recycle.Bin",
        sys_drive + "System Volume Information",
    };

    std::string pf = get_env_utf8(L"ProgramFiles");
    if (!pf.empty()) prefix_blocked.push_back(normalize_path_lexical(pf));
    std::string pf86 = get_env_utf8(L"ProgramFiles(x86)");
    if (!pf86.empty()) prefix_blocked.push_back(normalize_path_lexical(pf86));
    std::string pd = get_env_utf8(L"ProgramData");
    if (!pd.empty()) prefix_blocked.push_back(normalize_path_lexical(pd));

    std::string home = user_home.empty() ? get_env_utf8(L"USERPROFILE") : user_home;
    home = normalize_path_lexical(home);
    if (!home.empty()) {
        prefix_blocked.push_back(home + "\\AppData");
    }
    std::string local_app = get_env_utf8(L"LOCALAPPDATA");
    if (!local_app.empty()) {
        prefix_blocked.push_back(normalize_path_lexical(local_app) + "\\Programs\\DirLocker");
    }
    for (const auto& ex : extra_protected) {
        if (!ex.empty()) prefix_blocked.push_back(normalize_path_lexical(ex));
    }

    for (const auto& blk : prefix_blocked) {
        if (is_same_or_subpath(norm, blk)) {
            return reject("禁止锁定系统目录或程序受保护目录: " + blk);
        }
        // Also do not lock an ancestor of critical dirs (e.g. C:\Users if USERPROFILE is inside it)
        if (is_same_or_subpath(blk, norm)) {
            return reject("禁止锁定包含系统或受保护路径的上级目录");
        }
    }

    std::vector<std::string> exact_blocked = {
        sys_drive + "Users"
    };
    if (!home.empty()) {
        exact_blocked.push_back(home);
        exact_blocked.push_back(home + "\\Desktop");
        exact_blocked.push_back(home + "\\桌面");
        exact_blocked.push_back(home + "\\Documents");
        exact_blocked.push_back(home + "\\Downloads");
    }
    for (const auto& blk : exact_blocked) {
        if (paths_equal(norm, blk)) {
            return reject("禁止直接锁定用户主目录或桌面根目录本身（可锁定其子目录）");
        }
    }
#else
    if (norm == "/") return reject("禁止锁定根目录 /");
    constexpr char kMountVaultSuffix[] = "/.dirlocker_vault_do_not_delete";
    constexpr size_t kMountVaultSuffixLen = sizeof(kMountVaultSuffix) - 1;
    if (norm == kMountVaultSuffix ||
        norm.find("/.dirlocker_vault_do_not_delete/") != std::string::npos ||
        (norm.size() >= kMountVaultSuffixLen &&
         norm.compare(norm.size() - kMountVaultSuffixLen, kMountVaultSuffixLen, kMountVaultSuffix) == 0)) {
        return reject("禁止锁定目录密匣系统数据保护区");
    }

    std::vector<std::string> prefix_blocked = {
        "/bin", "/boot", "/dev", "/etc", "/lib", "/lib32", "/lib64", "/libx32",
        "/proc", "/run", "/sbin", "/snap", "/sys", "/usr",
        "/var/lib/dirlocker"
    };

    std::string home = user_home.empty() ? detect_user_home() : user_home;
    home = normalize_path_lexical(home);
    if (!home.empty()) {
        prefix_blocked.push_back(home + "/.config");
        prefix_blocked.push_back(home + "/.local");
        prefix_blocked.push_back(home + "/.cache");
        prefix_blocked.push_back(home + "/.ssh");
        prefix_blocked.push_back(home + "/.gnupg");
    }
    for (const auto& ex : extra_protected) {
        if (!ex.empty()) prefix_blocked.push_back(normalize_path_lexical(ex));
    }

    for (const auto& blk : prefix_blocked) {
        if (is_same_or_subpath(norm, blk)) {
            return reject("禁止锁定系统目录或受保护配置目录: " + blk);
        }
        if (is_same_or_subpath(blk, norm)) {
            return reject("禁止锁定包含系统或受保护目录的上级目录");
        }
    }

    std::vector<std::string> exact_blocked = {
        "/home", "/root", "/tmp", "/var", "/var/lib", "/opt", "/srv", "/media", "/mnt"
    };
    if (!home.empty()) {
        exact_blocked.push_back(home);
        exact_blocked.push_back(home + "/Desktop");
        exact_blocked.push_back(home + "/桌面");
        exact_blocked.push_back(home + "/Documents");
        exact_blocked.push_back(home + "/文档");
        exact_blocked.push_back(home + "/Downloads");
        exact_blocked.push_back(home + "/下载");
    }
    for (const auto& blk : exact_blocked) {
        if (paths_equal(norm, blk)) {
            return reject("禁止直接锁定系统顶层目录、用户主目录或桌面根目录本身（可锁定其子目录）");
        }
    }
#endif

    return false;
}

bool canonicalize_existing_dir(const std::string& raw_path, std::string& out, std::string* err) {
    std::string lex = normalize_path_lexical(raw_path);
    if (lex.empty()) {
        if (err) *err = "路径为空";
        return false;
    }

#ifdef _WIN32
    std::wstring wlex = utf8_to_wide(lex);
    DWORD attr = GetFileAttributesW(wlex.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = "目录不存在或无法访问";
        return false;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (err) *err = "目标不是目录";
        return false;
    }
    if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (err) *err = "不支持锁定符号链接或挂载点目录";
        return false;
    }
    DWORD needed = GetFullPathNameW(wlex.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        if (err) *err = "无法解析完整路径";
        return false;
    }
    std::wstring wfull(needed, L'\0');
    DWORD got = GetFullPathNameW(wlex.c_str(), needed, &wfull[0], nullptr);
    if (got == 0 || got >= needed) {
        if (err) *err = "解析完整路径失败";
        return false;
    }
    wfull.resize(got);
    out = normalize_path_lexical(wide_to_utf8(wfull));
    return true;
#else
    struct stat st{};
    if (lstat(lex.c_str(), &st) != 0) {
        if (err) *err = std::string("目录不存在或无法访问: ") + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        if (err) *err = "不支持锁定符号链接（软链接）";
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (err) *err = "目标不是目录";
        return false;
    }
    char resolved[PATH_MAX] = {};
    if (!realpath(lex.c_str(), resolved)) {
        if (err) *err = std::string("无法规范化路径: ") + std::strerror(errno);
        return false;
    }
    out = normalize_path_lexical(resolved);

    // Re-check resolved path is not a mount point root (e.g. /mnt/usb)
    if (out != "/") {
        size_t slash = out.rfind('/');
        std::string parent = (slash == 0) ? "/" : out.substr(0, slash);
        struct stat pst{};
        if (lstat(parent.c_str(), &pst) == 0 && pst.st_dev != st.st_dev) {
            if (err) *err = "不支持直接锁定磁盘挂载点根目录（可锁定其子目录）";
            return false;
        }
    }
    return true;
#endif
}

PathCheckResult validate_lockable_directory(
    const std::string& raw_path,
    const std::vector<std::string>& existing_paths,
    const std::string& user_home,
    const std::vector<std::string>& extra_protected) {
    PathCheckResult res;
    std::string err;
    if (!canonicalize_existing_dir(raw_path, res.canonical_path, &err)) {
        res.error = err;
        return res;
    }

    std::string reason;
    if (is_protected_system_path(res.canonical_path, user_home, extra_protected, &reason)) {
        res.error = reason;
        return res;
    }

    for (const auto& ex : existing_paths) {
        if (paths_equal(res.canonical_path, ex)) {
            res.error = "该目录已在 DirLocker 管理列表中";
            return res;
        }
        if (paths_overlap(res.canonical_path, ex)) {
            res.error = "不允许嵌套锁定已受保护目录的上级或下级目录: " + ex;
            return res;
        }
    }

    res.ok = true;
    return res;
}

}  // namespace dirlocker
