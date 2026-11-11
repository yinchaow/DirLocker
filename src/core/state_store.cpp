#include "state_store.hpp"

#include "crypto.hpp"
#include "json_mini.hpp"
#include "path_guard.hpp"

#include <array>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dirlocker {

namespace {

bool ensure_parent_dir_exists(const std::string& file_path) {
#ifdef _WIN32
    size_t pos = file_path.find_last_of("\\/");
    if (pos == std::string::npos) return true;
    std::string dir = file_path.substr(0, pos);
    if (dir.empty()) return true;

    std::wstring wdir = utf8_to_wide(dir);
    for (size_t i = 0; i < wdir.size(); ++i) {
        if (wdir[i] == L'\\' || wdir[i] == L'/') {
            if (i == 2 && wdir[1] == L':') continue;
            if (i <= 1) continue;
            std::wstring sub = wdir.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
    if (!CreateDirectoryW(wdir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
#else
    size_t pos = file_path.rfind('/');
    if (pos == std::string::npos || pos == 0) return true;
    std::string dir = file_path.substr(0, pos);
    for (size_t i = 1; i < dir.size(); ++i) {
        if (dir[i] == '/') {
            std::string sub = dir.substr(0, i);
            if (mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    chmod(dir.c_str(), 0700);
    return true;
#endif
}

bool read_all_file(const std::string& path, std::string& content, bool& exists, std::string* err) {
#ifdef _WIN32
    std::wstring wpath = utf8_to_wide(path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            exists = false;
            return true;
        }
        if (err) *err = "无法打开状态文件 (错误码 " + std::to_string(e) + ")";
        return false;
    }
    exists = true;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        if (err) *err = "状态文件大小异常";
        return false;
    }
    content.resize(static_cast<size_t>(sz.QuadPart));
    DWORD total = 0;
    while (total < static_cast<DWORD>(sz.QuadPart)) {
        DWORD read_now = 0;
        if (!ReadFile(h, &content[total], static_cast<DWORD>(sz.QuadPart) - total, &read_now, nullptr)) {
            CloseHandle(h);
            if (err) *err = "读取状态文件失败";
            return false;
        }
        if (read_now == 0) break;
        total += read_now;
    }
    content.resize(total);
    CloseHandle(h);
    return true;
#else
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            exists = false;
            return true;
        }
        if (err) *err = std::string("无法打开状态文件: ") + std::strerror(errno);
        return false;
    }
    exists = true;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        close(fd);
        if (err) *err = "状态文件大小异常";
        return false;
    }
    content.clear();
    content.reserve(static_cast<size_t>(st.st_size));
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            if (err) *err = std::string("读取状态文件失败: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) break;
        content.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return true;
#endif
}

bool write_atomic_file(const std::string& path, const std::string& content, std::string* err) {
    if (!ensure_parent_dir_exists(path)) {
        if (err) *err = "无法创建状态文件所在目录";
        return false;
    }

#ifdef _WIN32
    std::string tmp_path = path + ".tmp." + std::to_string(GetCurrentProcessId());
    std::wstring wtmp = utf8_to_wide(tmp_path);
    std::wstring wpath = utf8_to_wide(path);

    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "创建临时状态文件失败 (错误码 " + std::to_string(GetLastError()) + ")";
        return false;
    }
    size_t written = 0;
    while (written < content.size()) {
        DWORD chunk = static_cast<DWORD>(content.size() - written);
        DWORD out = 0;
        if (!WriteFile(h, content.data() + written, chunk, &out, nullptr)) {
            CloseHandle(h);
            DeleteFileW(wtmp.c_str());
            if (err) *err = "写入临时状态文件失败";
            return false;
        }
        written += out;
    }
    FlushFileBuffers(h);
    CloseHandle(h);

    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD e = GetLastError();
        DeleteFileW(wtmp.c_str());
        if (err) *err = "替换状态文件失败 (错误码 " + std::to_string(e) + ")";
        return false;
    }
    return true;
#else
    std::string tmp_path = path + ".tmp." + std::to_string(getpid());
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (err) *err = std::string("创建临时状态文件失败: ") + std::strerror(errno);
        return false;
    }
    size_t written = 0;
    while (written < content.size()) {
        ssize_t n = write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp_path.c_str());
            if (err) *err = std::string("写入临时状态文件失败: ") + std::strerror(errno);
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path.c_str());
        if (err) *err = std::string("落盘同步失败: ") + std::strerror(errno);
        return false;
    }
    close(fd);

    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        if (err) *err = std::string("替换状态文件失败: ") + std::strerror(errno);
        return false;
    }

    size_t pos = path.rfind('/');
    if (pos != std::string::npos) {
        std::string parent = (pos == 0) ? "/" : path.substr(0, pos);
        int dfd = open(parent.c_str(), O_RDONLY);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return true;
#endif
}

}  // namespace

StateStore::StateStore(std::string file_path) : file_path_(std::move(file_path)) {}

namespace {

bool parse_state_json(const std::string& content,
                      std::string& master_hash,
                      AppSettings& settings,
                      std::vector<DirEntry>& entries,
                      std::string* err) {
    JsonValue root;
    std::string perr;
    if (!JsonValue::parse(content, root, &perr) || !root.is_object()) {
        if (err) *err = "状态文件 JSON 解析失败: " + perr;
        return false;
    }

    std::string mh = root.get("master_hash").as_string();
    AppSettings st_out;
    const JsonValue& st = root.get("settings");
    if (st.is_object()) {
        int relock_min = static_cast<int>(st.get("auto_relock_minutes").as_int(5));
        if (relock_min < 0) relock_min = 0;
        if (relock_min > 1440) relock_min = 1440;
        st_out.auto_relock_minutes = relock_min;
        st_out.relock_on_screen_lock = st.get("relock_on_screen_lock").as_bool(true);
        std::string hk = st.get("hotkey").as_string_or("Ctrl+Alt+L");
        if (!hk.empty()) st_out.hotkey = hk;
    }

    std::vector<DirEntry> parsed_entries;
    const JsonValue& arr = root.get("entries");
    if (arr.is_array()) {
        for (const auto& item : arr.as_array()) {
            if (!item.is_object()) continue;
            DirEntry e;
            e.id = item.get("id").as_string();
            e.path = normalize_path_lexical(item.get("path").as_string());
            if (e.path.empty()) continue;
            if (e.id.empty()) e.id = StateStore::generate_id();
            e.locked = item.get("locked").as_bool(false);
            e.use_custom_password = item.get("use_custom_password").as_bool(false);
            e.custom_hash = item.get("custom_hash").as_string();
            e.orig_mode = static_cast<uint32_t>(item.get("orig_mode").as_int(0755));
            e.orig_uid = static_cast<uint32_t>(item.get("orig_uid").as_int(0));
            e.orig_gid = static_cast<uint32_t>(item.get("orig_gid").as_int(0));
            e.set_immutable = item.get("set_immutable").as_bool(false);
            e.win_sid = item.get("win_sid").as_string();
            e.vault_path = item.get("vault_path").as_string();
            e.stub_path = item.get("stub_path").as_string();
            e.in_place_acl = item.get("in_place_acl").as_bool(false);
            e.win_orig_attrs = static_cast<uint32_t>(item.get("win_orig_attrs").as_int(0));
            e.created_at = item.get("created_at").as_int(0);
            e.last_unlocked_at = item.get("last_unlocked_at").as_int(0);
            parsed_entries.push_back(std::move(e));
        }
    }

    master_hash = std::move(mh);
    settings = std::move(st_out);
    entries = std::move(parsed_entries);
    return true;
}

}  // namespace

bool StateStore::load(std::string* err) {
    entries_.clear();
    master_hash_.clear();
    settings_ = AppSettings{};

    std::string primary_err;
    std::string content;
    bool exists = false;
    bool primary_ok = read_all_file(file_path_, content, exists, &primary_err);
    if (primary_ok && exists && !content.empty()) {
        if (parse_state_json(content, master_hash_, settings_, entries_, &primary_err)) {
            return true;
        }
    }

    // Attempt recovery from hidden backup file if primary state file was deleted or damaged.
    std::string bak_path = file_path_ + ".bak";
    std::string bak_content;
    bool bak_exists = false;
    if (read_all_file(bak_path, bak_content, bak_exists, nullptr) && bak_exists && !bak_content.empty()) {
        if (parse_state_json(bak_content, master_hash_, settings_, entries_, nullptr)) {
#ifdef _WIN32
            SetFileAttributesW(utf8_to_wide(file_path_).c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
            write_atomic_file(file_path_, bak_content, nullptr);
            return true;
        }
    }

    if (primary_ok && (!exists || content.empty())) {
        return true;
    }
    if (err) *err = primary_err.empty() ? "读取状态文件失败" : primary_err;
    return false;
}

bool StateStore::save(std::string* err) const {
    JsonValue root = JsonValue::object();
    root.set("version", 1);
    root.set("master_hash", master_hash_);

    JsonValue st = JsonValue::object();
    st.set("auto_relock_minutes", settings_.auto_relock_minutes);
    st.set("relock_on_screen_lock", settings_.relock_on_screen_lock);
    st.set("hotkey", settings_.hotkey);
    root.set("settings", std::move(st));

    JsonValue arr = JsonValue::array();
    for (const auto& e : entries_) {
        JsonValue item = JsonValue::object();
        item.set("id", e.id);
        item.set("path", e.path);
        item.set("locked", e.locked);
        item.set("use_custom_password", e.use_custom_password);
        item.set("custom_hash", e.custom_hash);
        item.set("orig_mode", e.orig_mode);
        item.set("orig_uid", e.orig_uid);
        item.set("orig_gid", e.orig_gid);
        item.set("set_immutable", e.set_immutable);
        item.set("win_sid", e.win_sid);
        item.set("vault_path", e.vault_path);
        item.set("stub_path", e.stub_path);
        item.set("in_place_acl", e.in_place_acl);
        item.set("win_orig_attrs", e.win_orig_attrs);
        item.set("created_at", e.created_at);
        item.set("last_unlocked_at", e.last_unlocked_at);
        arr.push_back(std::move(item));
    }
    root.set("entries", std::move(arr));

    std::string serialized = root.dump(2) + "\n";
#ifdef _WIN32
    SetFileAttributesW(utf8_to_wide(file_path_).c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    if (!write_atomic_file(file_path_, serialized, err)) {
        return false;
    }

    std::string bak_path = file_path_ + ".bak";
#ifdef _WIN32
    SetFileAttributesW(utf8_to_wide(bak_path).c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    if (write_atomic_file(bak_path, serialized, nullptr)) {
#ifdef _WIN32
        SetFileAttributesW(utf8_to_wide(bak_path).c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
    }
    return true;
}

bool StateStore::set_master_password(const std::string& password) {
    std::string h = hash_password(password);
    if (h.empty()) return false;
    master_hash_ = std::move(h);
    return true;
}

bool StateStore::verify_master_password(const std::string& password) const {
    if (master_hash_.empty()) return false;
    return verify_password(password, master_hash_);
}

bool StateStore::verify_entry_password(const DirEntry& entry, const std::string& password) const {
    if (entry.use_custom_password) {
        if (entry.custom_hash.empty()) return false;
        return verify_password(password, entry.custom_hash);
    }
    return verify_master_password(password);
}

DirEntry* StateStore::find_by_path(const std::string& canonical_path) {
    for (auto& e : entries_) {
        if (paths_equal(e.path, canonical_path)) return &e;
    }
    return nullptr;
}

const DirEntry* StateStore::find_by_path(const std::string& canonical_path) const {
    for (const auto& e : entries_) {
        if (paths_equal(e.path, canonical_path)) return &e;
    }
    return nullptr;
}

DirEntry* StateStore::find_by_id(const std::string& id) {
    for (auto& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<std::string> StateStore::all_paths() const {
    std::vector<std::string> res;
    res.reserve(entries_.size());
    for (const auto& e : entries_) res.push_back(e.path);
    return res;
}

std::string StateStore::generate_id() {
    std::array<uint8_t, 8> buf{};
    random_bytes(buf.data(), buf.size());
    return bytes_to_hex(buf.data(), buf.size());
}

int64_t StateStore::now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace dirlocker

