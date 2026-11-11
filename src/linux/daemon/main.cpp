#include "../../core/crypto.hpp"
#include "../../core/json_mini.hpp"
#include "../../core/path_guard.hpp"
#include "../../core/protocol.hpp"
#include "../../core/state_store.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <mutex>
#include <pwd.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace dirlocker;

constexpr const char* kRunDir = "/run/dirlocker";
constexpr const char* kSocketPath = "/run/dirlocker/dirlockerd.sock";
constexpr const char* kStateDir = "/var/lib/dirlocker";
constexpr const char* kVaultBaseDir = "/var/lib/dirlocker/vault";
constexpr const char* kMountVaultFolder = ".dirlocker_vault_do_not_delete";

std::mutex g_state_mutex;
volatile sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

bool set_inode_flag(const std::string& path, int flag_mask, bool enable, bool& changed_flag) {
    changed_flag = false;
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;

    int flags = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) != 0) {
        close(fd);
        return false;
    }
    int new_flags = enable ? (flags | flag_mask) : (flags & ~flag_mask);
    if (new_flags == flags) {
        close(fd);
        changed_flag = enable;
        return true;
    }
    if (ioctl(fd, FS_IOC_SETFLAGS, &new_flags) != 0) {
        close(fd);
        return false;
    }
    close(fd);
    changed_flag = true;
    return true;
}

bool set_immutable_flag(const std::string& path, bool enable, bool& changed_flag) {
    return set_inode_flag(path, FS_IMMUTABLE_FL, enable, changed_flag);
}

bool ensure_dir_mode(const char* path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return false;
    }
    return chmod(path, mode) == 0;
}

bool ensure_vault_dir_root(const std::string& path) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
    }
    if (chown(path.c_str(), 0, 0) != 0) return false;
    return chmod(path.c_str(), 0700) == 0;
}

std::string decode_mount_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() &&
            in[i + 1] >= '0' && in[i + 1] <= '7' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            int val = ((in[i + 1] - '0') << 6) | ((in[i + 2] - '0') << 3) | (in[i + 3] - '0');
            out.push_back(static_cast<char>(val));
            i += 3;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string find_enclosing_mount_root(const std::string& target_path, dev_t target_dev) {
    std::ifstream ifs("/proc/mounts");
    std::string line;
    std::string best;
    while (std::getline(ifs, line)) {
        size_t p1 = line.find(' ');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find(' ', p1 + 1);
        if (p2 == std::string::npos) continue;
        std::string mnt = normalize_path_lexical(decode_mount_escape(line.substr(p1 + 1, p2 - (p1 + 1))));
        if (mnt.empty()) continue;
        if (mnt.rfind("/proc", 0) == 0 || mnt.rfind("/sys", 0) == 0 || mnt.rfind("/dev", 0) == 0) {
            continue;
        }
        if (!is_same_or_subpath(target_path, mnt)) continue;
        struct stat mst{};
        if (lstat(mnt.c_str(), &mst) == 0 && S_ISDIR(mst.st_mode) && mst.st_dev == target_dev) {
            if (best.empty() || mnt.size() > best.size()) {
                best = mnt;
            }
        }
    }
    return best;
}

bool resolve_linux_vault_dir(const std::string& target_path,
                             uint32_t uid,
                             const std::string& entry_id,
                             std::string& vault_dir_out,
                             std::string& err) {
    struct stat target_st{};
    if (lstat(target_path.c_str(), &target_st) != 0) {
        err = std::string("读取目录属性失败: ") + std::strerror(errno);
        return false;
    }

    std::string mnt = find_enclosing_mount_root(target_path, target_st.st_dev);
    if (mnt == "/") {
        if (ensure_vault_dir_root(kVaultBaseDir)) {
            struct stat vst{};
            if (lstat(kVaultBaseDir, &vst) == 0 && vst.st_dev == target_st.st_dev) {
                std::string uid_dir = std::string(kVaultBaseDir) + "/uid_" + std::to_string(uid);
                if (!ensure_vault_dir_root(uid_dir)) {
                    err = std::string("创建用户保护区失败: ") + std::strerror(errno);
                    return false;
                }
                vault_dir_out = uid_dir + "/" + entry_id;
                return true;
            }
        }
    }

    if (mnt.empty()) {
        if (ensure_vault_dir_root(kVaultBaseDir)) {
            struct stat vst{};
            if (lstat(kVaultBaseDir, &vst) == 0 && vst.st_dev == target_st.st_dev) {
                std::string uid_dir = std::string(kVaultBaseDir) + "/uid_" + std::to_string(uid);
                if (!ensure_vault_dir_root(uid_dir)) {
                    err = std::string("创建用户保护区失败: ") + std::strerror(errno);
                    return false;
                }
                vault_dir_out = uid_dir + "/" + entry_id;
                return true;
            }
        }
        err = "无法定位目标目录所在的本地磁盘挂载点";
        return false;
    }

    std::string mnt_vault = (mnt == "/")
        ? (std::string("/") + kMountVaultFolder)
        : (mnt + "/" + kMountVaultFolder);
    if (!ensure_vault_dir_root(mnt_vault)) {
        err = std::string("无法在磁盘挂载点创建保护区（当前文件系统可能只读或不支持 Linux 权限）: ") +
              std::strerror(errno);
        return false;
    }
    std::string uid_dir = mnt_vault + "/uid_" + std::to_string(uid);
    if (!ensure_vault_dir_root(uid_dir)) {
        err = std::string("创建保护区子目录失败: ") + std::strerror(errno);
        return false;
    }
    vault_dir_out = uid_dir + "/" + entry_id;
    return true;
}

std::string sanitize_desktop_value(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '\r' || c == '\n' || c == '\0') continue;
        out.push_back(c);
    }
    return out;
}

bool write_desktop_stub(const std::string& stub_path,
                        const std::string& orig_dir_path,
                        const std::string& entry_id,
                        uint32_t orig_uid,
                        uint32_t orig_gid,
                        std::string& err) {
    struct stat est{};
    if (lstat(stub_path.c_str(), &est) == 0) {
        err = "目标旁已存在同名 .desktop 入口文件，无法加锁";
        return false;
    }

    size_t slash = orig_dir_path.rfind('/');
    std::string folder_name = (slash == std::string::npos) ? orig_dir_path : orig_dir_path.substr(slash + 1);
    if (folder_name.empty()) folder_name = "加密目录";

    std::string content;
    content += "[Desktop Entry]\n";
    content += "Version=1.0\n";
    content += "Type=Application\n";
    content += "Name=" + sanitize_desktop_value(folder_name) + "\n";
    content += "Comment=目录密匣加密目录（双击输入密码解锁）\n";
    content += "Exec=/usr/bin/dirlocker --unlock-id " + entry_id + "\n";
    content += "Icon=dirlocker-folder\n";
    content += "Terminal=false\n";
    content += "StartupNotify=true\n";
    content += "X-DirLocker-Id=" + entry_id + "\n";

    int fd = open(stub_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (fd < 0) {
        err = std::string("创建加密入口失败: ") + std::strerror(errno);
        return false;
    }
    size_t written = 0;
    while (written < content.size()) {
        ssize_t n = write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = std::string("写入加密入口失败: ") + std::strerror(errno);
            close(fd);
            unlink(stub_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    fsync(fd);
    close(fd);

    if (chown(stub_path.c_str(), orig_uid, orig_gid) != 0 || chmod(stub_path.c_str(), 0755) != 0) {
        err = std::string("设置加密入口权限失败: ") + std::strerror(errno);
        unlink(stub_path.c_str());
        return false;
    }
    // Use FS_APPEND_FL (chattr +a) rather than root/immutable so Peony reports canWrite()==true
    // (hiding the top-left readonly lock emblem) while the kernel still blocks delete/rename/truncate.
    bool dummy = false;
    set_inode_flag(stub_path, FS_APPEND_FL, true, dummy);
    return true;
}

void repair_legacy_desktop_stub(const DirEntry& e) {
    if (!e.locked || e.stub_path.size() <= 8 || e.id.empty()) return;
    const std::string ext = ".desktop";
    if (e.stub_path.size() < ext.size()) return;
    for (size_t i = 0; i < ext.size(); ++i) {
        char a = e.stub_path[e.stub_path.size() - ext.size() + i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != ext[i]) return;
    }

    int fd = open(e.stub_path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    std::string text(buf, static_cast<size_t>(n));
    if (text.find("Exec=/usr/bin/dirlocker --unlock %f") == std::string::npos) {
        return;
    }

    bool dummy = false;
    set_inode_flag(e.stub_path, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
    unlink(e.stub_path.c_str());
    std::string stub_err;
    write_desktop_stub(e.stub_path, e.path, e.id, e.orig_uid, e.orig_gid, stub_err);
}

bool apply_linux_lock(DirEntry& entry, uint32_t expected_uid, std::string& err) {
    struct stat st{};
    if (lstat(entry.path.c_str(), &st) != 0) {
        err = std::string("目录不存在或无法访问: ") + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        err = "目标不是真实目录";
        return false;
    }

    if (!entry.locked) {
        if (expected_uid != 0 && st.st_uid != expected_uid) {
            err = "只能锁定当前登录用户自己拥有的目录";
            return false;
        }
        entry.orig_mode = static_cast<uint32_t>(st.st_mode & 07777);
        entry.orig_uid = static_cast<uint32_t>(st.st_uid);
        entry.orig_gid = static_cast<uint32_t>(st.st_gid);
    }

    std::string stub_path = entry.path + ".desktop";
    struct stat stub_st{};
    if (lstat(stub_path.c_str(), &stub_st) == 0) {
        err = "目录旁已存在同名 .desktop 入口文件，无法加锁";
        return false;
    }

    std::string vault_dir;
    if (!resolve_linux_vault_dir(entry.path, entry.orig_uid, entry.id, vault_dir, err)) {
        return false;
    }
    struct stat vst{};
    if (lstat(vault_dir.c_str(), &vst) == 0) {
        err = "保护区内存在同名冲突条目，请先解锁或检查状态";
        return false;
    }

    // Clear immutable temporarily if it happens to be set, before moving.
    bool dummy = false;
    set_immutable_flag(entry.path, false, dummy);

    if (std::rename(entry.path.c_str(), vault_dir.c_str()) != 0) {
        err = std::string("移动目录至保护区失败: ") + std::strerror(errno);
        return false;
    }

    if (chown(vault_dir.c_str(), 0, 0) != 0 || chmod(vault_dir.c_str(), 0700) != 0) {
        int saved_errno = errno;
        if (chown(vault_dir.c_str(), entry.orig_uid, entry.orig_gid) != 0) {}
        if (chmod(vault_dir.c_str(), static_cast<mode_t>(entry.orig_mode & 07777)) != 0) {}
        std::rename(vault_dir.c_str(), entry.path.c_str());
        err = std::string("设置保护区目录权限失败: ") + std::strerror(saved_errno);
        return false;
    }

    std::string stub_err;
    if (!write_desktop_stub(stub_path, entry.path, entry.id, entry.orig_uid, entry.orig_gid, stub_err)) {
        if (chown(vault_dir.c_str(), entry.orig_uid, entry.orig_gid) != 0) {}
        if (chmod(vault_dir.c_str(), static_cast<mode_t>(entry.orig_mode & 07777)) != 0) {}
        std::rename(vault_dir.c_str(), entry.path.c_str());
        err = stub_err;
        return false;
    }

    bool imm_ok = false;
    set_immutable_flag(vault_dir, true, imm_ok);
    entry.vault_path = std::move(vault_dir);
    entry.stub_path = std::move(stub_path);
    entry.set_immutable = imm_ok;
    entry.locked = true;
    return true;
}

bool apply_linux_unlock(DirEntry& entry, std::string& err) {
    if (!entry.vault_path.empty()) {
        struct stat vst{};
        if (lstat(entry.vault_path.c_str(), &vst) != 0) {
            err = std::string("保护区目录不存在或已被外部移除: ") + std::strerror(errno);
            return false;
        }
        if (S_ISLNK(vst.st_mode) || !S_ISDIR(vst.st_mode)) {
            err = "保护区目标不是真实目录";
            return false;
        }

        struct stat est{};
        if (lstat(entry.path.c_str(), &est) == 0) {
            err = "原路径已存在同名文件或目录，请先移走后再解锁";
            return false;
        }

        bool dummy = false;
        set_immutable_flag(entry.vault_path, false, dummy);

        if (std::rename(entry.vault_path.c_str(), entry.path.c_str()) != 0) {
            err = std::string("从保护区移回原目录失败: ") + std::strerror(errno);
            set_immutable_flag(entry.vault_path, true, dummy);
            return false;
        }

        if (chown(entry.path.c_str(), entry.orig_uid, entry.orig_gid) != 0 ||
            chmod(entry.path.c_str(), static_cast<mode_t>(entry.orig_mode & 07777)) != 0) {
            err = std::string("恢复目录属主或权限失败: ") + std::strerror(errno);
            return false;
        }

        std::string stub = entry.stub_path.empty() ? (entry.path + ".desktop") : entry.stub_path;
        set_inode_flag(stub, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
        unlink(stub.c_str());

        entry.vault_path.clear();
        entry.stub_path.clear();
        entry.set_immutable = false;
        entry.locked = false;
        entry.last_unlocked_at = StateStore::now_epoch();
        return true;
    }

    // Legacy in-place permission unlock compatibility (from <= v1.5.0)
    struct stat st{};
    if (lstat(entry.path.c_str(), &st) != 0) {
        err = std::string("目录不存在或已被外部移除: ") + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        err = "目标不是真实目录";
        return false;
    }

    bool dummy = false;
    set_immutable_flag(entry.path, false, dummy);
    entry.set_immutable = false;

    if (chown(entry.path.c_str(), entry.orig_uid, entry.orig_gid) != 0) {
        err = std::string("恢复目录属主失败: ") + std::strerror(errno);
        return false;
    }
    if (chmod(entry.path.c_str(), static_cast<mode_t>(entry.orig_mode & 07777)) != 0) {
        err = std::string("恢复目录权限失败: ") + std::strerror(errno);
        return false;
    }

    entry.locked = false;
    entry.last_unlocked_at = StateStore::now_epoch();
    return true;
}

bool ends_with_ci(const std::string& p, const std::string& suffix) {
    if (p.size() <= suffix.size()) return false;
    std::string ext = p.substr(p.size() - suffix.size());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext == suffix;
}

bool strip_stub_suffix_ci(const std::string& p, std::string& out_trimmed) {
    if (ends_with_ci(p, ".desktop")) {
        out_trimmed = p.substr(0, p.size() - 8);
        return true;
    }
    if (ends_with_ci(p, ".dlock")) {
        out_trimmed = p.substr(0, p.size() - 6);
        return true;
    }
    return false;
}

bool ends_with_stub_ext_ci(const std::string& p) {
    return ends_with_ci(p, ".desktop") || ends_with_ci(p, ".dlock");
}

std::string resolve_parent_realpath_join(const std::string& raw_path) {
    std::string lex = normalize_path_lexical(raw_path);
    size_t slash = lex.rfind('/');
    if (slash == std::string::npos) return lex;
    std::string parent = (slash == 0) ? "/" : lex.substr(0, slash);
    std::string base = lex.substr(slash + 1);
    char resolved[PATH_MAX] = {};
    if (realpath(parent.c_str(), resolved)) {
        std::string rp = normalize_path_lexical(resolved);
        return (rp == "/") ? ("/" + base) : (rp + "/" + base);
    }
    return lex;
}

DirEntry* find_managed_entry(StateStore& store, const std::string& raw_path) {
    std::string canonical;
    if (canonicalize_existing_dir(raw_path, canonical)) {
        if (DirEntry* e = store.find_by_path(canonical)) return e;
    }
    std::string resolved_lex = resolve_parent_realpath_join(raw_path);
    if (DirEntry* e = store.find_by_path(resolved_lex)) return e;

    std::string lex = normalize_path_lexical(raw_path);
    if (DirEntry* e = store.find_by_path(lex)) return e;

    std::string trimmed_resolved;
    std::string trimmed_lex;
    bool has_trimmed_resolved = strip_stub_suffix_ci(resolved_lex, trimmed_resolved);
    bool has_trimmed_lex = strip_stub_suffix_ci(lex, trimmed_lex);

    for (auto& e : store.entries_mut()) {
        if (!e.stub_path.empty() &&
            (paths_equal(e.stub_path, resolved_lex) || paths_equal(e.stub_path, lex))) {
            return &e;
        }
        if (has_trimmed_resolved && paths_equal(e.path, trimmed_resolved)) {
            return &e;
        }
        if (has_trimmed_lex && paths_equal(e.path, trimmed_lex)) {
            return &e;
        }
    }
    return nullptr;
}

std::string get_user_home_for_uid(uint32_t uid) {
    struct passwd pwd{};
    struct passwd* res = nullptr;
    std::vector<char> buf(4096);
    if (getpwuid_r(uid, &pwd, buf.data(), buf.size(), &res) == 0 && res && res->pw_dir) {
        return res->pw_dir;
    }
    return {};
}

std::string state_path_for_uid(uint32_t uid) {
    return std::string(kStateDir) + "/uid_" + std::to_string(uid) + ".json";
}

JsonValue serialize_public_state(const StateStore& store) {
    JsonValue data = JsonValue::object();
    data.set("has_master_password", store.has_master_password());

    JsonValue st = JsonValue::object();
    st.set("auto_relock_minutes", store.settings().auto_relock_minutes);
    st.set("relock_on_screen_lock", store.settings().relock_on_screen_lock);
    st.set("hotkey", store.settings().hotkey);
    data.set("settings", std::move(st));

    JsonValue arr = JsonValue::array();
    for (const auto& e : store.entries()) {
        JsonValue item = JsonValue::object();
        item.set("id", e.id);
        item.set("path", e.path);
        item.set("locked", e.locked);
        item.set("use_custom_password", e.use_custom_password);
        item.set("created_at", e.created_at);
        item.set("last_unlocked_at", e.last_unlocked_at);
        arr.push_back(std::move(item));
    }
    data.set("entries", std::move(arr));
    return data;
}

std::string handle_request_json(const JsonValue& req, uint32_t caller_uid) {
    const std::string action = req.get("action").as_string();
    const std::string user_home = get_user_home_for_uid(caller_uid);

    std::lock_guard<std::mutex> lock(g_state_mutex);

    StateStore store(state_path_for_uid(caller_uid));
    std::string io_err;
    if (!store.load(&io_err)) {
        return make_response(false, "加载状态文件失败: " + io_err);
    }

    if (action == "status") {
        for (const auto& e : store.entries()) {
            repair_legacy_desktop_stub(e);
        }
        return make_response(true, "", serialize_public_state(store));
    }

    if (action == "query") {
        std::string by_id = req.get("id").as_string();
        std::string raw_path = req.get("path").as_string();
        JsonValue data = JsonValue::object();
        const DirEntry* matched = nullptr;
        if (!by_id.empty()) {
            matched = store.find_by_id(by_id);
            if (!matched) {
                data.set("managed", false);
                data.set("locked", false);
                data.set("protectable", false);
                data.set("reason", "未找到该加密入口对应的目录记录");
                return make_response(true, "", std::move(data));
            }
        } else {
            matched = find_managed_entry(store, raw_path);
        }
        if (matched) {
            data.set("canonical_path", matched->path);
            data.set("managed", true);
            data.set("locked", matched->locked);
            data.set("use_custom_password", matched->use_custom_password);
            data.set("protectable", true);
            return make_response(true, "", std::move(data));
        }

        std::string canonical;
        std::string cerr;
        if (!canonicalize_existing_dir(raw_path, canonical, &cerr)) {
            data.set("managed", false);
            data.set("locked", false);
            data.set("protectable", false);
            data.set("reason", ends_with_stub_ext_ci(raw_path) ? "未找到该加密入口对应的目录记录" : cerr);
            return make_response(true, "", std::move(data));
        }

        data.set("canonical_path", canonical);
        auto check = validate_lockable_directory(canonical, store.all_paths(), user_home);
        data.set("managed", false);
        data.set("locked", false);
        data.set("protectable", check.ok);
        if (!check.ok) data.set("reason", check.error);
        return make_response(true, "", std::move(data));
    }

    if (action == "set_master_password") {
        std::string old_pw = req.get("old_password").as_string();
        std::string new_pw = req.get("new_password").as_string();
        if (new_pw.size() < 4) {
            return make_response(false, "主密码长度至少需要 4 个字符");
        }
        if (store.has_master_password() && !store.verify_master_password(old_pw)) {
            return make_response(false, "原主密码不正确");
        }
        if (!store.set_master_password(new_pw)) {
            return make_response(false, "生成密码哈希失败");
        }
        if (!store.save(&io_err)) {
            return make_response(false, "保存主密码失败: " + io_err);
        }
        return make_response(true, "主密码已更新", serialize_public_state(store));
    }

    if (action == "update_settings") {
        std::string pw = req.get("password").as_string();
        if (!store.has_master_password() || !store.verify_master_password(pw)) {
            return make_response(false, "需要验证主密码才能修改安全设置");
        }
        int mins = static_cast<int>(req.get("auto_relock_minutes").as_int(5));
        if (mins < 0) mins = 0;
        if (mins > 1440) mins = 1440;
        store.settings_mut().auto_relock_minutes = mins;
        store.settings_mut().relock_on_screen_lock = req.get("relock_on_screen_lock").as_bool(true);
        std::string hk = req.get("hotkey").as_string_or("Ctrl+Alt+L");
        if (!hk.empty()) store.settings_mut().hotkey = hk;
        if (!store.save(&io_err)) {
            return make_response(false, "保存设置失败: " + io_err);
        }
        return make_response(true, "设置已保存", serialize_public_state(store));
    }

    if (action == "verify_master") {
        std::string pw = req.get("password").as_string();
        if (!store.has_master_password() || !store.verify_master_password(pw)) {
            return make_response(false, "主密码不正确");
        }
        return make_response(true, "验证通过");
    }

    if (action == "add_and_lock") {
        if (!store.has_master_password()) {
            return make_response(false, "请先设置主密码");
        }
        std::string raw_path = req.get("path").as_string();
        bool use_custom = req.get("use_custom_password").as_bool(false);
        std::string custom_pw = req.get("password").as_string();
        std::string cerr;

        if (DirEntry* existing = find_managed_entry(store, raw_path)) {
            if (existing->locked) {
                return make_response(true, "目录已处于锁定状态", serialize_public_state(store));
            }
            if (!apply_linux_lock(*existing, caller_uid, cerr)) {
                return make_response(false, cerr);
            }
            if (!store.save(&io_err)) {
                return make_response(false, "保存状态失败: " + io_err);
            }
            return make_response(true, "目录已重新锁定", serialize_public_state(store));
        }

        auto check = validate_lockable_directory(raw_path, store.all_paths(), user_home);
        if (!check.ok) {
            return make_response(false, check.error);
        }

        DirEntry entry;
        entry.id = StateStore::generate_id();
        entry.path = check.canonical_path;
        entry.use_custom_password = use_custom;
        entry.created_at = StateStore::now_epoch();
        if (use_custom) {
            if (custom_pw.size() < 4) {
                return make_response(false, "独立密码长度至少需要 4 个字符");
            }
            entry.custom_hash = hash_password(custom_pw);
            if (entry.custom_hash.empty()) {
                return make_response(false, "生成密码哈希失败");
            }
        }

        if (!apply_linux_lock(entry, caller_uid, cerr)) {
            return make_response(false, cerr);
        }

        store.entries_mut().push_back(std::move(entry));
        if (!store.save(&io_err)) {
            return make_response(false, "保存状态失败: " + io_err);
        }
        return make_response(true, "目录已锁定", serialize_public_state(store));
    }

    if (action == "lock") {
        std::string raw_path = req.get("path").as_string();
        std::string cerr;
        DirEntry* entry = find_managed_entry(store, raw_path);
        if (!entry) {
            return make_response(false, "该目录尚未加入目录密匣管理列表");
        }
        if (entry->locked) {
            return make_response(true, "目录已处于锁定状态", serialize_public_state(store));
        }
        if (!apply_linux_lock(*entry, caller_uid, cerr)) {
            return make_response(false, cerr);
        }
        if (!store.save(&io_err)) {
            return make_response(false, "保存状态失败: " + io_err);
        }
        return make_response(true, "目录已锁定", serialize_public_state(store));
    }

    if (action == "lock_all") {
        int locked_now = 0;
        std::vector<std::string> failed;
        for (auto& e : store.entries_mut()) {
            if (e.locked) continue;
            std::string cerr;
            if (apply_linux_lock(e, caller_uid, cerr)) {
                ++locked_now;
            } else {
                failed.push_back(e.path + " (" + cerr + ")");
            }
        }
        if (!store.save(&io_err)) {
            return make_response(false, "保存状态失败: " + io_err);
        }
        JsonValue data = serialize_public_state(store);
        data.set("locked_count", locked_now);
        if (!failed.empty()) {
            return make_response(false, "部分目录锁定失败: " + failed.front(), std::move(data));
        }
        return make_response(true, "已锁定 " + std::to_string(locked_now) + " 个目录", std::move(data));
    }

    if (action == "unlock") {
        std::string raw_path = req.get("path").as_string();
        std::string password = req.get("password").as_string();
        std::string cerr;
        DirEntry* entry = find_managed_entry(store, raw_path);
        if (!entry) {
            return make_response(false, "未找到该目录的锁定记录");
        }
        if (!store.verify_entry_password(*entry, password)) {
            return make_response(false, "密码错误");
        }
        if (entry->locked) {
            if (!apply_linux_unlock(*entry, cerr)) {
                return make_response(false, cerr);
            }
            if (!store.save(&io_err)) {
                return make_response(false, "保存状态失败: " + io_err);
            }
        }
        return make_response(true, "解锁成功", serialize_public_state(store));
    }

    if (action == "change_dir_password") {
        std::string raw_path = req.get("path").as_string();
        std::string current_pw = req.get("current_password").as_string();
        bool use_custom = req.get("use_custom_password").as_bool(false);
        std::string new_pw = req.get("new_password").as_string();

        DirEntry* entry = find_managed_entry(store, raw_path);
        if (!entry) {
            return make_response(false, "未找到该目录记录");
        }
        if (!store.verify_entry_password(*entry, current_pw)) {
            return make_response(false, "当前密码验证失败");
        }
        if (use_custom) {
            if (new_pw.size() < 4) {
                return make_response(false, "独立密码长度至少需要 4 个字符");
            }
            std::string h = hash_password(new_pw);
            if (h.empty()) return make_response(false, "生成密码哈希失败");
            entry->use_custom_password = true;
            entry->custom_hash = std::move(h);
        } else {
            entry->use_custom_password = false;
            entry->custom_hash.clear();
        }
        if (!store.save(&io_err)) {
            return make_response(false, "保存设置失败: " + io_err);
        }
        return make_response(true, "密码方式已更新", serialize_public_state(store));
    }

    if (action == "remove") {
        std::string raw_path = req.get("path").as_string();
        std::string password = req.get("password").as_string();
        DirEntry* target = find_managed_entry(store, raw_path);
        if (target) {
            if (!store.verify_entry_password(*target, password)) {
                return make_response(false, "密码错误，无法移除受保护目录");
            }
            if (target->locked) {
                std::string cerr;
                struct stat st{};
                bool has_target = !target->vault_path.empty() || lstat(target->path.c_str(), &st) == 0;
                if (has_target) {
                    if (!apply_linux_unlock(*target, cerr)) {
                        return make_response(false, "解锁恢复原目录失败，已中止移除: " + cerr);
                    }
                }
            }
            auto& vec = store.entries_mut();
            for (size_t i = 0; i < vec.size(); ++i) {
                if (&vec[i] == target) {
                    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (!store.save(&io_err)) {
                return make_response(false, "保存状态失败: " + io_err);
            }
            return make_response(true, "已解锁并移出管理列表", serialize_public_state(store));
        }
        return make_response(false, "未找到该目录记录");
    }

    return make_response(false, "未知指令: " + action);
}

void handle_client(int client_fd) {
    timeval tv{};
    tv.tv_sec = 3;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct ucred cred{};
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0) {
        close(client_fd);
        return;
    }

    std::string line;
    line.reserve(1024);
    char ch = 0;
    while (line.size() < 65536) {
        ssize_t n = read(client_fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(client_fd);
            return;
        }
        if (n == 0 || ch == '\n') break;
        line.push_back(ch);
    }

    std::string resp;
    JsonValue req;
    std::string perr;
    if (!JsonValue::parse(line, req, &perr) || !req.is_object()) {
        resp = make_response(false, "无效 JSON 请求: " + perr);
    } else {
        resp = handle_request_json(req, cred.uid);
    }

    size_t sent = 0;
    while (sent < resp.size()) {
        ssize_t n = write(client_fd, resp.data() + sent, resp.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        sent += static_cast<size_t>(n);
    }
    close(client_fd);
}

}  // namespace

int main() {
    if (geteuid() != 0) {
        std::cerr << "dirlockerd must run as root (systemd service).\n";
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // Do not set SA_RESTART so accept() unblocks immediately on SIGINT/SIGTERM
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (!ensure_dir_mode(kStateDir, 0700) || !ensure_dir_mode(kRunDir, 0755)) {
        std::cerr << "Failed to initialize runtime/state directories: "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    unlink(kSocketPath);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kSocketPath);

    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind() failed: " << std::strerror(errno) << "\n";
        close(srv);
        return 1;
    }

    if (chmod(kSocketPath, 0666) != 0) {
        std::cerr << "chmod(socket) failed: " << std::strerror(errno) << "\n";
        close(srv);
        return 1;
    }

    if (listen(srv, 32) != 0) {
        std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
        close(srv);
        return 1;
    }

    while (g_running) {
        int cfd = accept(srv, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread(handle_client, cfd).detach();
    }

    close(srv);
    unlink(kSocketPath);
    return 0;
}
