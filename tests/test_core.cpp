#include "../src/core/crypto.hpp"
#include "../src/core/json_mini.hpp"
#include "../src/core/path_guard.hpp"
#include "../src/core/protocol.hpp"
#include "../src/core/security_util.hpp"
#include "../src/core/state_store.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__       \
                      << ": " << #cond << "\n";                                  \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

using namespace dirlocker;

void test_crypto_vectors() {
    // NIST SHA-256("abc")
    auto h1 = sha256("abc");
    CHECK(bytes_to_hex(h1.data(), h1.size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Empty string SHA-256
    auto h0 = sha256("");
    CHECK(bytes_to_hex(h0.data(), h0.size()) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // RFC 4231 Test Case 1: key = 0x0b * 20, data = "Hi There"
    std::vector<uint8_t> key1(20, 0x0b);
    const char* data1 = "Hi There";
    auto mac1 = hmac_sha256(key1.data(), key1.size(),
                            reinterpret_cast<const uint8_t*>(data1), std::strlen(data1));
    CHECK(bytes_to_hex(mac1.data(), mac1.size()) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 6070 / PBKDF2-HMAC-SHA256 vector: password="password", salt="salt", c=1, dkLen=32
    const uint8_t salt1[] = {'s', 'a', 'l', 't'};
    auto pb1 = pbkdf2_hmac_sha256("password", salt1, 4, 1, 32);
    CHECK(bytes_to_hex(pb1.data(), pb1.size()) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");

    // Password hashing & verification
    std::string encoded = hash_password("Secret@123", 10000);
    CHECK(!encoded.empty());
    CHECK(verify_password("Secret@123", encoded));
    CHECK(!verify_password("Secret@124", encoded));
    CHECK(!verify_password("", encoded));
    CHECK(!verify_password("Secret@123", "pbkdf2_sha256$100$aa$bb"));
}

void test_json() {
    JsonValue obj = JsonValue::object();
    obj.set("name", "机密\"测试\"\n目录");
    obj.set("count", 42);
    obj.set("flag", true);
    JsonValue arr = JsonValue::array();
    arr.push_back("a");
    arr.push_back(123);
    obj.set("items", std::move(arr));

    std::string dumped = obj.dump(2);
    JsonValue parsed;
    std::string err;
    CHECK(JsonValue::parse(dumped, parsed, &err));
    CHECK(parsed.is_object());
    CHECK(parsed.get("name").as_string() == "机密\"测试\"\n目录");
    CHECK(parsed.get("count").as_int() == 42);
    CHECK(parsed.get("flag").as_bool() == true);
    CHECK(parsed.get("items").as_array().size() == 2);

    // Unicode surrogate pair parsing (\uD83D\uDD12 -> lock emoji UTF-8)
    JsonValue emoji;
    CHECK(JsonValue::parse("{\"k\":\"\\uD83D\\uDD12\"}", emoji));
    CHECK(emoji.get("k").as_string() == "\xF0\x9F\x94\x92");

    // Invalid JSON rejected
    JsonValue bad;
    CHECK(!JsonValue::parse("{\"a\":1,}", bad));
    CHECK(!JsonValue::parse("{\"a\":1} trailing", bad));
}

void test_path_guard_and_state() {
#ifdef _WIN32
    CHECK(normalize_path_lexical("d:/work//secret\\") == "D:\\work\\secret");
    CHECK(paths_equal("D:\\Work\\Secret", "d:/work/secret/"));
    CHECK(is_same_or_subpath("D:\\Work\\Secret\\Sub", "d:\\work\\secret"));
    CHECK(!is_same_or_subpath("D:\\Work\\Secret2", "d:\\work\\secret"));
#else
    CHECK(normalize_path_lexical("/home/user//docs/.") == "/home/user/docs");
    CHECK(normalize_path_lexical("/home/user/docs/") == "/home/user/docs");
    CHECK(is_same_or_subpath("/home/user/docs/a", "/home/user/docs"));
    CHECK(!is_same_or_subpath("/home/user/docs2", "/home/user/docs"));
    CHECK(paths_overlap("/a/b", "/a/b/c"));
    CHECK(paths_overlap("/a/b/c", "/a/b"));

    std::string reason;
    CHECK(is_protected_system_path("/", "/home/user", {}, &reason));
    CHECK(is_protected_system_path("/etc", "/home/user", {}, &reason));
    CHECK(is_protected_system_path("/home/user", "/home/user", {}, &reason));
    CHECK(is_protected_system_path("/home/user/Desktop", "/home/user", {}, &reason));
    CHECK(is_protected_system_path("/home/user/.config/foo", "/home/user", {}, &reason));
    CHECK(!is_protected_system_path("/home/user/Desktop/Secret", "/home/user", {}, &reason));

    // Create temp directory tree to test validation, symlink rejection, and StateStore persistence.
    char tmpl[] = "/tmp/dirlocker_test_XXXXXX";
    char* base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    std::string base_dir(base);

    std::string target_dir = base_dir + "/secret_folder";
    std::string nested_dir = target_dir + "/inner";
    std::string link_dir = base_dir + "/sym_secret";
    CHECK(mkdir(target_dir.c_str(), 0755) == 0);
    CHECK(mkdir(nested_dir.c_str(), 0755) == 0);
    CHECK(symlink(target_dir.c_str(), link_dir.c_str()) == 0);

    // Symlink must be rejected
    auto r_sym = validate_lockable_directory(link_dir, {}, "/home/nobody");
    CHECK(!r_sym.ok);

    // Real dir succeeds
    auto r_ok = validate_lockable_directory(target_dir, {}, "/home/nobody");
    CHECK(r_ok.ok);

    // Nested subdir inside already-locked dir must be rejected
    auto r_nest = validate_lockable_directory(nested_dir, {r_ok.canonical_path}, "/home/nobody");
    CHECK(!r_nest.ok);

    // StateStore roundtrip & password verification
    std::string state_file = base_dir + "/nested_cfg/state.json";
    {
        StateStore store(state_file);
        CHECK(store.set_master_password("Master#2026"));

        DirEntry e1;
        e1.id = StateStore::generate_id();
        e1.path = r_ok.canonical_path;
        e1.locked = true;
        e1.use_custom_password = false;
        e1.orig_mode = 0750;
        e1.vault_path = "/tmp/fake_vault/id1";
        e1.stub_path = r_ok.canonical_path + ".dlock";
        e1.in_place_acl = false;
        e1.win_orig_attrs = 0x10;
        store.entries_mut().push_back(e1);

        DirEntry e2;
        e2.id = StateStore::generate_id();
        e2.path = base_dir + "/other_folder";
        e2.locked = false;
        e2.use_custom_password = true;
        e2.custom_hash = hash_password("Custom@999", 10000);
        store.entries_mut().push_back(e2);

        store.settings_mut().auto_relock_minutes = 12;
        store.settings_mut().require_master_on_open = true;
        store.settings_mut().allow_in_place_fallback = true;
        store.settings_mut().hotkey = "ctrl + alt + shift + k";

        std::string err;
        CHECK(store.save(&err));
    }

    {
        StateStore loaded(state_file);
        std::string err;
        CHECK(loaded.load(&err));
        CHECK(loaded.has_master_password());
        CHECK(loaded.verify_master_password("Master#2026"));
        CHECK(!loaded.verify_master_password("Wrong"));
        CHECK(loaded.settings().auto_relock_minutes == 12);
        CHECK(loaded.settings().require_master_on_open == true);
        CHECK(loaded.settings().allow_in_place_fallback == true);
        CHECK(loaded.settings().hotkey == "Ctrl+Alt+Shift+K");
        CHECK(loaded.entries().size() == 2);

        const DirEntry* f1 = loaded.find_by_path(r_ok.canonical_path);
        CHECK(f1 != nullptr);
        CHECK(f1->orig_mode == 0750);
        CHECK(f1->vault_path == "/tmp/fake_vault/id1");
        CHECK(f1->stub_path == r_ok.canonical_path + ".dlock");
        CHECK(f1->in_place_acl == false);
        CHECK(is_protected_system_path("/data/.dirlocker_vault_do_not_delete", "/home/nobody"));
        CHECK(is_protected_system_path("/data/.dirlocker_vault_do_not_delete/uid_1000", "/home/nobody"));
        CHECK(f1->win_orig_attrs == 0x10);
        CHECK(loaded.verify_entry_password(*f1, "Master#2026"));
        CHECK(!loaded.verify_entry_password(*f1, "Custom@999"));

        const DirEntry* f2 = loaded.find_by_path(base_dir + "/other_folder");
        CHECK(f2 != nullptr);
        CHECK(loaded.verify_entry_password(*f2, "Custom@999"));
        CHECK(!loaded.verify_entry_password(*f2, "Master#2026"));
    }

    // Verify automatic recovery from .bak when state.json is deleted or corrupted
    {
        unlink(state_file.c_str());
        StateStore recovered(state_file);
        std::string err;
        CHECK(recovered.load(&err));
        CHECK(recovered.has_master_password());
        CHECK(recovered.entries().size() == 2);

        FILE* corrupt = fopen(state_file.c_str(), "wb");
        CHECK(corrupt != nullptr);
        fwrite("{corrupted", 1, 10, corrupt);
        fclose(corrupt);

        StateStore recovered2(state_file);
        CHECK(recovered2.load(&err));
        CHECK(recovered2.entries().size() == 2);
    }

    // Verify legacy settings schema (with removed relock_on_screen_lock) still loads cleanly
    {
        FILE* legacy = fopen(state_file.c_str(), "wb");
        CHECK(legacy != nullptr);
        const char* legacy_json =
            "{\"version\":1,\"settings\":{\"auto_relock_minutes\":8,"
            "\"relock_on_screen_lock\":true,\"hotkey\":\"Alt+Shift+L\"},\"entries\":[]}";
        fwrite(legacy_json, 1, strlen(legacy_json), legacy);
        fclose(legacy);

        StateStore leg_store(state_file);
        std::string err;
        CHECK(leg_store.load(&err));
        CHECK(leg_store.settings().auto_relock_minutes == 8);
        CHECK(leg_store.settings().require_master_on_open == false);
        CHECK(leg_store.settings().allow_in_place_fallback == false);
        CHECK(leg_store.settings().hotkey == "Alt+Shift+L");
    }

    // Cleanup temp directory
    unlink(link_dir.c_str());
    rmdir(nested_dir.c_str());
    rmdir(target_dir.c_str());
    unlink(state_file.c_str());
    unlink((state_file + ".bak").c_str());
    rmdir((base_dir + "/nested_cfg").c_str());
    rmdir(base_dir.c_str());
#endif
}

void test_hotkey_and_auth_throttle() {
    using dirlocker::AuthThrottle;
    using dirlocker::HotkeySpec;

    HotkeySpec spec;
    std::string err;
    CHECK(HotkeySpec::parse("ctrl + alt + shift + f9", spec, &err));
    CHECK(spec.to_string() == "Ctrl+Alt+Shift+F9");
    CHECK(HotkeySpec::parse("Ctrl+0", spec, &err));
    CHECK(spec.to_string() == "Ctrl+0");
    CHECK(HotkeySpec::normalize("invalid", "Ctrl+Alt+L") == "Ctrl+Alt+L");

    CHECK(!HotkeySpec::parse("", spec, &err));
    CHECK(!HotkeySpec::parse("L", spec, &err));
    CHECK(!HotkeySpec::parse("Shift+L", spec, &err));
    CHECK(!HotkeySpec::parse("Alt+F4", spec, &err));
    CHECK(!HotkeySpec::parse("Ctrl+Alt+Tab", spec, &err));
    CHECK(!HotkeySpec::parse("Ctrl+Alt+A+B", spec, &err));
    CHECK(HotkeySpec::parse("Ctrl+Alt+F4", spec, &err));

    AuthThrottle th;
    int64_t t = 100000;
    for (int i = 0; i < 4; ++i) {
        CHECK(th.record_failure(t) == 0);
        CHECK(th.remaining_cooldown_seconds(t) == 0);
    }
    CHECK(th.record_failure(t) == 30);
    CHECK(th.remaining_cooldown_seconds(t) == 30);
    CHECK(th.remaining_cooldown_seconds(t + 10000) == 20);
    CHECK(th.remaining_cooldown_seconds(t + 30000) == 0);

    CHECK(th.record_failure(t + 30000) == 60);
    CHECK(th.remaining_cooldown_seconds(t + 30000) == 60);
    CHECK(th.record_failure(t + 90000) == 300);
    CHECK(th.record_failure(t + 400000) == 300);
    CHECK(th.consecutive_failures() == 8);

    th.record_success();
    CHECK(th.consecutive_failures() == 0);
    CHECK(th.remaining_cooldown_seconds(t + 400000) == 0);
}

}  // namespace

int main() {
    test_crypto_vectors();
    test_json();
    test_path_guard_and_state();
    test_hotkey_and_auth_throttle();
    std::cout << "All DirLocker core unit tests passed.\n";
    return 0;
}
