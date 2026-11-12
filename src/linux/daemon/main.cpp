#include "../../core/crypto.hpp"
#include "../../core/json_mini.hpp"
#include "../../core/path_guard.hpp"
#include "../../core/protocol.hpp"
#include "../../core/security_util.hpp"
#include "../../core/state_store.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace dirlocker;

constexpr const char* kRunDir = "/run/dirlocker";
constexpr const char* kSocketPath = "/run/dirlocker/dirlockerd.sock";
constexpr const char* kStateDir = "/var/lib/dirlocker";
constexpr const char* kVaultBaseDir = "/var/lib/dirlocker/vault";
constexpr const char* kMountVaultFolder = ".dirlocker_vault_do_not_delete";

std::mutex g_state_mutex;
std::unordered_map<uint32_t, AuthThrottle> g_auth_throttles;
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

std::string readlink_str(const std::string& p) {
    std::vector<char> buf(PATH_MAX + 1, '\0');
    ssize_t n = readlink(p.c_str(), buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    return std::string(buf.data(), static_cast<size_t>(n));
}

std::string strip_deleted_suffix(std::string p) {
    static constexpr const char* kDel = " (deleted)";
    size_t klen = std::strlen(kDel);
    if (p.size() > klen && p.compare(p.size() - klen, klen, kDel) == 0) {
        p.resize(p.size() - klen);
    }
    return p;
}

bool is_same_or_subpath_posix(const std::string& candidate, const std::string& base_dir) {
    if (candidate.empty() || base_dir.empty() || candidate[0] != '/') return false;
    if (candidate == base_dir) return true;
    if (base_dir == "/") return true;
    if (candidate.size() > base_dir.size() &&
        candidate.compare(0, base_dir.size(), base_dir) == 0 &&
        candidate[base_dir.size()] == '/') {
        return true;
    }
    return false;
}

std::string read_proc_name(const char* pid_str) {
    std::string comm_path = std::string("/proc/") + pid_str + "/comm";
    std::ifstream ifs(comm_path);
    std::string line;
    if (std::getline(ifs, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) return line;
    }
    std::string exe = readlink_str(std::string("/proc/") + pid_str + "/exe");
    if (!exe.empty()) {
        size_t pos = exe.find_last_of('/');
        return (pos == std::string::npos) ? exe : exe.substr(pos + 1);
    }
    return std::string("PID ") + pid_str;
}

std::vector<std::string> find_blocking_processes_for_dir(const std::string& target_path) {
    std::vector<std::string> procs;
    std::unordered_set<std::string> seen;

    std::string canon_target = target_path;
    char rp[PATH_MAX] = {};
    if (realpath(target_path.c_str(), rp)) {
        canon_target = rp;
    }
    while (canon_target.size() > 1 && canon_target.back() == '/') {
        canon_target.pop_back();
    }

    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return procs;

    pid_t self_pid = getpid();
    struct dirent* ent = nullptr;
    while ((ent = readdir(proc_dir)) != nullptr && procs.size() < 6) {
        const char* name = ent->d_name;
        if (!std::isdigit(static_cast<unsigned char>(name[0]))) continue;
        bool all_digit = true;
        for (size_t i = 0; name[i] != '\0'; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                all_digit = false;
                break;
            }
        }
        if (!all_digit) continue;

        pid_t pid = static_cast<pid_t>(std::strtol(name, nullptr, 10));
        if (pid <= 1 || pid == self_pid) continue;

        bool hit = false;
        std::string cwd = readlink_str(std::string("/proc/") + name + "/cwd");
        if (!cwd.empty() && is_same_or_subpath_posix(strip_deleted_suffix(cwd), canon_target)) {
            hit = true;
        }

        if (!hit) {
            std::string fd_dir_path = std::string("/proc/") + name + "/fd";
            DIR* fd_dir = opendir(fd_dir_path.c_str());
            if (fd_dir) {
                struct dirent* fd_ent = nullptr;
                while ((fd_ent = readdir(fd_dir)) != nullptr) {
                    if (fd_ent->d_name[0] == '.') continue;
                    std::string target = readlink_str(fd_dir_path + "/" + fd_ent->d_name);
                    if (!target.empty() && target[0] == '/' &&
                        is_same_or_subpath_posix(strip_deleted_suffix(target), canon_target)) {
                        hit = true;
                        break;
                    }
                }
                closedir(fd_dir);
            }
        }

        if (hit) {
            std::string pname = read_proc_name(name);
            if (!pname.empty() && seen.insert(pname).second) {
                procs.push_back(std::move(pname));
            }
        }
    }
    closedir(proc_dir);
    return procs;
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

    std::vector<std::string> blockers = find_blocking_processes_for_dir(entry.path);
    if (!blockers.empty()) {
        std::string joined;
        for (size_t i = 0; i < blockers.size(); ++i) {
            if (i > 0) joined += "、";
            joined += blockers[i];
        }
        err = "目录或其中文件正被以下程序占用，无法上锁：" + joined +
              "\n请先关闭相关文档、程序或终端窗口后再试（注：文本编辑器等不持续占用句柄的程序不影响上锁）。";
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

bool dir_exists_posix(const std::string& p) {
    if (p.empty()) return false;
    struct stat st{};
    return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool file_or_link_exists_posix(const std::string& p) {
    if (p.empty()) return false;
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

std::string unescape_proc_mount_field(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size()) {
            if (raw.compare(i, 4, "\\040") == 0) { out.push_back(' '); i += 3; continue; }
            if (raw.compare(i, 4, "\\011") == 0) { out.push_back('\t'); i += 3; continue; }
            if (raw.compare(i, 4, "\\012") == 0) { out.push_back('\n'); i += 3; continue; }
            if (raw.compare(i, 4, "\\134") == 0) { out.push_back('\\'); i += 3; continue; }
        }
        out.push_back(raw[i]);
    }
    return out;
}

std::vector<std::string> list_mounted_points() {
    std::vector<std::string> mounts;
    std::ifstream ifs("/proc/mounts");
    std::string dev, mnt;
    std::string rest;
    while (ifs >> dev >> mnt) {
        std::getline(ifs, rest);
        mounts.push_back(unescape_proc_mount_field(mnt));
    }
    return mounts;
}

bool is_linux_path_unmounted(const std::string& path, const std::vector<std::string>& mounts) {
    auto is_removable_prefix = [](const std::string& p) {
        return p.compare(0, 7, "/media/") == 0 ||
               p.compare(0, 11, "/run/media/") == 0 ||
               p.compare(0, 5, "/mnt/") == 0;
    };
    if (!is_removable_prefix(path)) return false;

    for (const auto& m : mounts) {
        if (m == "/" || m.empty()) continue;
        if (is_same_or_subpath_posix(path, m)) {
            return false;  // Deep mountpoint is active
        }
    }
    return !dir_exists_posix(path);
}

std::vector<std::string> collect_linux_vault_uid_dirs(uint32_t uid,
                                                      const std::vector<std::string>& mounts) {
    std::vector<std::string> dirs;
    std::unordered_set<std::string> seen;
    auto maybe_add = [&](const std::string& p) {
        if (p.empty()) return;
        if (seen.insert(p).second && dir_exists_posix(p)) {
            dirs.push_back(p);
        }
    };
    std::string uid_suffix = "/uid_" + std::to_string(uid);
    maybe_add(std::string(kVaultBaseDir) + uid_suffix);
    for (const auto& m : mounts) {
        if (m.empty()) continue;
        std::string base = (m == "/") ? (std::string("/") + kMountVaultFolder)
                                      : (m + "/" + kMountVaultFolder);
        maybe_add(base + uid_suffix);
    }
    return dirs;
}

std::string normalize_posix_strip(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool stub_has_unlock_id(const std::string& stub_path, const std::string& entry_id) {
    std::ifstream ifs(stub_path);
    if (!ifs) return false;
    std::string line, text;
    while (std::getline(ifs, line)) {
        text += line;
        text += '\n';
    }
    return text.find("--unlock-id " + entry_id) != std::string::npos;
}

JsonValue diagnose_linux_state(const StateStore& store, uint32_t uid) {
    std::vector<std::string> mounts = list_mounted_points();
    std::unordered_set<std::string> known_vaults;

    int total = 0;
    int ok_cnt = 0;
    int repairable_cnt = 0;
    int manual_cnt = 0;
    int unmounted_cnt = 0;
    int orphan_cnt = 0;
    JsonValue items = JsonValue::array();

    auto push_item = [&](const std::string& id, const std::string& kind,
                         const std::string& display_path, const std::string& vault_path,
                         const std::string& summary, const std::string& hint,
                         bool auto_repair, bool allow_remove) {
        JsonValue it = JsonValue::object();
        it.set("entry_id", id);
        it.set("kind", kind);
        it.set("display_path", display_path);
        it.set("vault_path", vault_path);
        it.set("summary", summary);
        it.set("repair_hint", hint);
        it.set("auto_repairable", auto_repair);
        it.set("allow_remove_record", allow_remove);
        items.push_back(std::move(it));
    };

    for (const auto& e : store.entries()) {
        ++total;
        if (!e.vault_path.empty()) {
            known_vaults.insert(normalize_posix_strip(e.vault_path));
        }

        if (is_linux_path_unmounted(e.path, mounts) ||
            (!e.vault_path.empty() && is_linux_path_unmounted(e.vault_path, mounts))) {
            ++unmounted_cnt;
            push_item(e.id, "unmounted_disk", e.path, e.vault_path,
                      "所在移动磁盘或挂载点当前未挂载",
                      "若为U盘或外置磁盘，请重新挂载后再体检", false, false);
            continue;
        }

        bool orig_exists = dir_exists_posix(e.path);
        bool vault_exists = !e.vault_path.empty() && dir_exists_posix(e.vault_path);

        if (e.locked) {
            if (orig_exists && vault_exists) {
                ++manual_cnt;
                push_item(e.id, "conflict_both_exist", e.path, e.vault_path,
                          "原路径与保护区同时存在同名目录（冲突）",
                          "为保数据安全不会自动删除，请管理员核对两端文件后再处理",
                          false, false);
                continue;
            }
            if (vault_exists && !orig_exists) {
                std::string stub = e.stub_path.empty() ? (e.path + ".desktop") : e.stub_path;
                if (!file_or_link_exists_posix(stub) || !stub_has_unlock_id(stub, e.id)) {
                    ++repairable_cnt;
                    push_item(e.id, "missing_stub", e.path, e.vault_path,
                              "保护区数据完好，但桌面 .desktop 锁标入口缺失或版本过旧",
                              "可一键补建 .desktop 入口并校验保护区权限",
                              true, false);
                } else {
                    ++ok_cnt;
                }
                continue;
            }
            if (!vault_exists && orig_exists) {
                ++repairable_cnt;
                push_item(e.id, "externally_restored", e.path, e.vault_path,
                          "目录已回到原位置，但状态文件仍标记为已锁定",
                          "可一键同步状态为“未锁定”并清理多余 .desktop 文件",
                          true, false);
                continue;
            }
            ++manual_cnt;
            push_item(e.id, "vault_missing", e.path, e.vault_path,
                      "原目录与保护区目录均不存在（可能被外部移除或磁盘变化）",
                      "确认无需保留记录后，可勾选“清理已失联记录”移出列表",
                      false, true);
            continue;
        }

        std::string stray1 = e.path + ".desktop";
        bool has_stray = file_or_link_exists_posix(stray1) ||
                         (!e.stub_path.empty() && file_or_link_exists_posix(e.stub_path));
        if (has_stray) {
            ++repairable_cnt;
            push_item(e.id, "stale_unlocked_stub", e.path, e.vault_path,
                      "目录处于已解锁状态，但旁边仍残留多余 .desktop 入口文件",
                      "可一键安全清理残留的 .desktop 入口文件",
                      true, false);
            continue;
        }
        if (!orig_exists) {
            ++manual_cnt;
            push_item(e.id, "missing_unlocked_dir", e.path, e.vault_path,
                      "未锁定目录已被外部移动或删除",
                      "若确认不再需要保护该目录，可勾选“清理已失联记录”移出列表",
                      false, true);
            continue;
        }
        ++ok_cnt;
    }

    for (const auto& uid_dir : collect_linux_vault_uid_dirs(uid, mounts)) {
        DIR* d = opendir(uid_dir.c_str());
        if (!d) continue;
        struct dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
            std::string sub = normalize_posix_strip(uid_dir + "/" + ent->d_name);
            if (!dir_exists_posix(sub)) continue;
            if (known_vaults.count(sub) == 0) {
                ++orphan_cnt;
                ++manual_cnt;
                push_item("", "orphan_vault_dir", sub, sub,
                          "保护区中存在未绑定状态记录的孤立数据目录（内部数据完好）",
                          "系统绝不会自动删除保护区数据，请参照《应急恢复说明》由管理员核实恢复",
                          false, false);
            }
        }
        closedir(d);
    }

    JsonValue out = JsonValue::object();
    out.set("total_entries", total);
    out.set("ok_count", ok_cnt);
    out.set("repairable_count", repairable_cnt);
    out.set("manual_count", manual_cnt);
    out.set("unmounted_count", unmounted_cnt);
    out.set("orphan_vault_count", orphan_cnt);
    out.set("items", std::move(items));
    return out;
}

bool verify_password_throttled(uint32_t uid,
                               const std::string& wrong_msg,
                               const std::function<bool()>& verify_fn,
                               std::string& fail_resp) {
    AuthThrottle& th = g_auth_throttles[uid];
    int64_t rem = th.remaining_cooldown_seconds();
    if (rem > 0) {
        JsonValue d = JsonValue::object();
        d.set("cooldown_seconds", rem);
        fail_resp = make_response(
            false, AuthThrottle::format_cooldown_error(static_cast<int>(rem)), std::move(d));
        return false;
    }
    if (verify_fn()) {
        th.record_success();
        return true;
    }
    th.record_failure();
    int64_t rem_now = th.remaining_cooldown_seconds();
    JsonValue d = JsonValue::object();
    std::string msg = wrong_msg;
    if (rem_now > 0) {
        d.set("cooldown_seconds", rem_now);
        msg += "\n" + AuthThrottle::format_cooldown_error(static_cast<int>(rem_now));
    }
    fail_resp = make_response(false, msg, std::move(d));
    return false;
}

JsonValue serialize_public_state(const StateStore& store) {
    JsonValue data = JsonValue::object();
    data.set("has_master_password", store.has_master_password());

    JsonValue st = JsonValue::object();
    st.set("auto_relock_minutes", store.settings().auto_relock_minutes);
    st.set("require_master_on_open", store.settings().require_master_on_open);
    st.set("hotkey", HotkeySpec::normalize(store.settings().hotkey, "Ctrl+Alt+L"));
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
        if (store.has_master_password()) {
            std::string fail_resp;
            if (!verify_password_throttled(
                    caller_uid, "原主密码不正确",
                    [&]() { return store.verify_master_password(old_pw); }, fail_resp)) {
                return fail_resp;
            }
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
        if (!store.has_master_password()) {
            return make_response(false, "请先设置主密码");
        }
        {
            std::string fail_resp;
            if (!verify_password_throttled(
                    caller_uid, "主密码错误，未保存设置",
                    [&]() { return store.verify_master_password(pw); }, fail_resp)) {
                return fail_resp;
            }
        }
        int mins = static_cast<int>(req.get("auto_relock_minutes").as_int(5));
        if (mins < 0) mins = 0;
        if (mins > 1440) mins = 1440;

        std::string raw_hk = req.get("hotkey").as_string_or("Ctrl+Alt+L");
        HotkeySpec hk_spec;
        std::string hk_err;
        if (!HotkeySpec::parse(raw_hk, hk_spec, &hk_err)) {
            return make_response(false, "快捷键格式无效: " + hk_err);
        }

        store.settings_mut().auto_relock_minutes = mins;
        store.settings_mut().require_master_on_open =
            req.get("require_master_on_open").as_bool(store.settings().require_master_on_open);
        store.settings_mut().hotkey = hk_spec.to_string();
        if (!store.save(&io_err)) {
            return make_response(false, "保存设置失败: " + io_err);
        }
        return make_response(true, "设置已保存", serialize_public_state(store));
    }

    if (action == "verify_master") {
        std::string pw = req.get("password").as_string();
        if (!store.has_master_password()) {
            return make_response(false, "尚未设置主密码");
        }
        std::string fail_resp;
        if (!verify_password_throttled(
                caller_uid, "主密码不正确",
                [&]() { return store.verify_master_password(pw); }, fail_resp)) {
            return fail_resp;
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
        JsonValue skipped_arr = JsonValue::array();
        for (auto& e : store.entries_mut()) {
            if (e.locked) continue;
            std::string cerr;
            if (apply_linux_lock(e, caller_uid, cerr)) {
                ++locked_now;
            } else {
                std::vector<std::string> procs = find_blocking_processes_for_dir(e.path);
                size_t slash = e.path.find_last_of('/');
                std::string bname = (slash == std::string::npos) ? e.path : e.path.substr(slash + 1);
                if (!procs.empty()) {
                    bname += " (" + procs[0] + (procs.size() > 1 ? " 等" : "") + ")";
                }
                skipped_arr.push_back(JsonValue(bname));
                failed.push_back(e.path + " (" + cerr + ")");
            }
        }
        if (locked_now > 0 && !store.save(&io_err)) {
            return make_response(false, "保存状态失败: " + io_err);
        }
        JsonValue data = serialize_public_state(store);
        data.set("locked_count", locked_now);
        data.set("skipped_count", static_cast<int>(failed.size()));
        data.set("skipped_items", std::move(skipped_arr));
        if (!failed.empty()) {
            std::string msg = (locked_now > 0
                                   ? ("已锁定 " + std::to_string(locked_now) + " 个目录，另有 ")
                                   : "") +
                              std::to_string(failed.size()) + " 个目录被占用跳过：" + failed.front();
            return make_response(false, msg, std::move(data));
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
        {
            std::string fail_resp;
            if (!verify_password_throttled(
                    caller_uid, "密码错误",
                    [&]() { return store.verify_entry_password(*entry, password); }, fail_resp)) {
                return fail_resp;
            }
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
        {
            std::string fail_resp;
            if (!verify_password_throttled(
                    caller_uid, "当前密码验证失败",
                    [&]() { return store.verify_entry_password(*entry, current_pw); }, fail_resp)) {
                return fail_resp;
            }
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
            {
                std::string fail_resp;
                if (!verify_password_throttled(
                        caller_uid, "密码错误，无法移除受保护目录",
                        [&]() { return store.verify_entry_password(*target, password); },
                        fail_resp)) {
                    return fail_resp;
                }
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

    if (action == "diagnose") {
        std::string pw = req.get("password").as_string();
        if (!store.has_master_password()) {
            return make_response(false, "请先设置主密码");
        }
        std::string fail_resp;
        if (!verify_password_throttled(
                caller_uid, "主密码错误，无权进行保护区体检",
                [&]() { return store.verify_master_password(pw); }, fail_resp)) {
            return fail_resp;
        }
        JsonValue rep = diagnose_linux_state(store, caller_uid);
        return make_response(true, "", std::move(rep));
    }

    if (action == "repair") {
        std::string pw = req.get("password").as_string();
        bool clean_dead = req.get("clean_dead_records").as_bool(false);
        if (!store.has_master_password()) {
            return make_response(false, "请先设置主密码");
        }
        std::string fail_resp;
        if (!verify_password_throttled(
                caller_uid, "主密码错误，无权执行修复操作",
                [&]() { return store.verify_master_password(pw); }, fail_resp)) {
            return fail_resp;
        }

        JsonValue pre = diagnose_linux_state(store, caller_uid);
        const JsonValue& items = pre.get("items");
        bool changed = false;
        int fixed_count = 0;
        JsonValue details = JsonValue::array();
        std::unordered_set<std::string> remove_ids;

        if (items.is_array()) {
            for (size_t i = 0; i < items.as_array().size(); ++i) {
                const JsonValue& it = items.as_array()[i];
                std::string eid = it.get("entry_id").as_string();
                std::string kind = it.get("kind").as_string();
                DirEntry* entry = eid.empty() ? nullptr : store.find_by_id(eid);
                if (!entry) continue;

                if (kind == "missing_stub") {
                    std::string stub = entry->stub_path.empty() ? (entry->path + ".desktop") : entry->stub_path;
                    bool dummy = false;
                    set_inode_flag(stub, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                    unlink(stub.c_str());
                    std::string serr;
                    if (write_desktop_stub(stub, entry->path, entry->id, entry->orig_uid, entry->orig_gid, serr)) {
                        if (chown(entry->vault_path.c_str(), 0, 0) != 0) {}
                        if (chmod(entry->vault_path.c_str(), 0700) != 0) {}
                        bool imm = false;
                        set_immutable_flag(entry->vault_path, true, imm);
                        entry->set_immutable = imm;
                        entry->stub_path = stub;
                        changed = true;
                        ++fixed_count;
                        details.push_back(JsonValue("已补建 .desktop 入口并校验保护区权限: " + entry->path));
                    }
                } else if (kind == "externally_restored") {
                    bool dummy = false;
                    if (!entry->stub_path.empty()) {
                        set_inode_flag(entry->stub_path, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                        unlink(entry->stub_path.c_str());
                    }
                    std::string def_stub = entry->path + ".desktop";
                    set_inode_flag(def_stub, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                    unlink(def_stub.c_str());
                    entry->locked = false;
                    entry->vault_path.clear();
                    entry->stub_path.clear();
                    changed = true;
                    ++fixed_count;
                    details.push_back(JsonValue("已同步外部恢复状态为未锁定: " + entry->path));
                } else if (kind == "stale_unlocked_stub") {
                    bool dummy = false;
                    if (!entry->stub_path.empty()) {
                        set_inode_flag(entry->stub_path, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                        unlink(entry->stub_path.c_str());
                    }
                    std::string def_stub = entry->path + ".desktop";
                    set_inode_flag(def_stub, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                    unlink(def_stub.c_str());
                    entry->stub_path.clear();
                    changed = true;
                    ++fixed_count;
                    details.push_back(JsonValue("已清理残留 .desktop 文件: " + entry->path));
                } else if ((kind == "vault_missing" || kind == "missing_unlocked_dir") && clean_dead) {
                    bool dummy = false;
                    if (!entry->stub_path.empty()) {
                        set_inode_flag(entry->stub_path, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                        unlink(entry->stub_path.c_str());
                    }
                    std::string def_stub = entry->path + ".desktop";
                    set_inode_flag(def_stub, FS_IMMUTABLE_FL | FS_APPEND_FL, false, dummy);
                    unlink(def_stub.c_str());
                    remove_ids.insert(entry->id);
                    changed = true;
                    ++fixed_count;
                    details.push_back(JsonValue("已移除失联记录: " + entry->path));
                }
            }
        }

        if (!remove_ids.empty()) {
            auto& vec = store.entries_mut();
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [&](const DirEntry& e) { return remove_ids.count(e.id) > 0; }),
                      vec.end());
        }
        if (changed && !store.save(&io_err)) {
            return make_response(false, "保存修复状态失败: " + io_err);
        }

        JsonValue data = diagnose_linux_state(store, caller_uid);
        data.set("repaired_count", fixed_count);
        data.set("details", std::move(details));
        data.set("state", serialize_public_state(store));
        return make_response(true,
                             fixed_count > 0
                                 ? ("已完成 " + std::to_string(fixed_count) + " 项安全修复")
                                 : "当前没有可自动修复的条目",
                             std::move(data));
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
