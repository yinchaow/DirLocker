#pragma once

#include <cstdint>
#include <string>

namespace dirlocker {

struct HotkeySpec {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    std::string key; // Normalized key token: "A"-"Z", "0"-"9", "F1"-"F12"

    bool is_valid() const { return !key.empty() && (ctrl || alt); }

    std::string to_string() const;

    static bool parse(const std::string& raw, HotkeySpec& out, std::string* err = nullptr);
    static std::string normalize(const std::string& raw,
                                 const std::string& fallback = "Ctrl+Alt+L");
};

// Non-destructive, time-only brute-force cooldown guard for password prompts.
// Failures 1..4 -> no wait; 5 -> 30s; 6 -> 60s; 7+ -> 300s cap. Any success resets to 0.
class AuthThrottle {
public:
    static int64_t steady_now_ms();

    int64_t remaining_cooldown_seconds() const {
        return remaining_cooldown_seconds(steady_now_ms());
    }
    int64_t remaining_cooldown_seconds(int64_t now_ms) const;

    // Records a failed password attempt and returns the triggered cooldown in seconds (0 if < 5).
    int record_failure() { return record_failure(steady_now_ms()); }
    int record_failure(int64_t now_ms);

    void record_success();

    int consecutive_failures() const { return consecutive_failures_; }

    static int cooldown_for_failures(int failures);
    static std::string format_cooldown_error(int remaining_sec);

private:
    int consecutive_failures_ = 0;
    int64_t cooldown_until_ms_ = 0;
};

} // namespace dirlocker

