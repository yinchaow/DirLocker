#include "core/security_util.hpp"

#include <cctype>
#include <chrono>
#include <sstream>
#include <vector>

namespace dirlocker {

namespace {

std::string trim_ascii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string to_upper_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool normalize_main_key(const std::string& upper, std::string& out_key) {
    if (upper.size() == 1) {
        char c = upper[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out_key.assign(1, c);
            return true;
        }
        return false;
    }
    if (upper.size() >= 2 && upper.size() <= 3 && upper[0] == 'F') {
        int fn = 0;
        for (size_t i = 1; i < upper.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(upper[i]))) {
                return false;
            }
            fn = fn * 10 + (upper[i] - '0');
        }
        if (fn >= 1 && fn <= 12) {
            out_key = "F" + std::to_string(fn);
            return true;
        }
    }
    return false;
}

} // namespace

std::string HotkeySpec::to_string() const {
    if (!is_valid()) return "";
    std::string out;
    if (ctrl) out += "Ctrl+";
    if (alt) out += "Alt+";
    if (shift) out += "Shift+";
    out += key;
    return out;
}

bool HotkeySpec::parse(const std::string& raw, HotkeySpec& out, std::string* err) {
    out = HotkeySpec{};
    std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        if (err) *err = "快捷键不能为空";
        return false;
    }

    std::vector<std::string> tokens;
    std::stringstream ss(trimmed);
    std::string part;
    while (std::getline(ss, part, '+')) {
        std::string tok = to_upper_ascii(trim_ascii(part));
        if (tok.empty()) {
            if (err) *err = "快捷键格式无效（存在空键段）";
            return false;
        }
        tokens.push_back(std::move(tok));
    }

    if (tokens.empty()) {
        if (err) *err = "快捷键不能为空";
        return false;
    }

    for (const std::string& tok : tokens) {
        if (tok == "CTRL" || tok == "CONTROL") {
            out.ctrl = true;
        } else if (tok == "ALT") {
            out.alt = true;
        } else if (tok == "SHIFT") {
            out.shift = true;
        } else {
            if (!out.key.empty()) {
                if (err) *err = "快捷键只能包含一个主键";
                return false;
            }
            if (!normalize_main_key(tok, out.key)) {
                if (err) *err = "主键仅支持字母 A-Z、数字 0-9 或功能键 F1-F12";
                return false;
            }
        }
    }

    if (!out.ctrl && !out.alt) {
        if (err) *err = "全局快捷键必须至少包含 Ctrl 或 Alt 修饰键";
        return false;
    }
    if (out.key.empty()) {
        if (err) *err = "请按下字母、数字或 F1-F12 主键完成组合";
        return false;
    }
    if (out.alt && !out.ctrl && !out.shift && out.key == "F4") {
        if (err) *err = "Alt+F4 为系统关闭窗口快捷键，请更换其他组合";
        return false;
    }
    return true;
}

std::string HotkeySpec::normalize(const std::string& raw, const std::string& fallback) {
    HotkeySpec spec;
    if (parse(raw, spec)) {
        return spec.to_string();
    }
    HotkeySpec fb;
    if (parse(fallback, fb)) {
        return fb.to_string();
    }
    return "Ctrl+Alt+K";
}

int64_t AuthThrottle::steady_now_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

int AuthThrottle::cooldown_for_failures(int failures) {
    if (failures < 5) return 0;
    if (failures == 5) return 30;
    if (failures == 6) return 60;
    return 300;
}

int64_t AuthThrottle::remaining_cooldown_seconds(int64_t now_ms) const {
    if (cooldown_until_ms_ <= now_ms) return 0;
    int64_t diff_ms = cooldown_until_ms_ - now_ms;
    return (diff_ms + 999) / 1000;
}

int AuthThrottle::record_failure(int64_t now_ms) {
    ++consecutive_failures_;
    int cooldown_sec = cooldown_for_failures(consecutive_failures_);
    if (cooldown_sec > 0) {
        int64_t target = now_ms + static_cast<int64_t>(cooldown_sec) * 1000;
        if (target > cooldown_until_ms_) {
            cooldown_until_ms_ = target;
        }
    }
    return cooldown_sec;
}

void AuthThrottle::record_success() {
    consecutive_failures_ = 0;
    cooldown_until_ms_ = 0;
}

std::string AuthThrottle::format_cooldown_error(int remaining_sec) {
    if (remaining_sec <= 0) return {};
    return "连续密码错误次数过多，为防暴力试探，请等待 " +
           std::to_string(remaining_sec) + " 秒后再试（不会删除或损坏您的目录数据）";
}

} // namespace dirlocker

