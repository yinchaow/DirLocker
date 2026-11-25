package core

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPBKDF2HashAndVerify(t *testing.T) {
	hash, err := HashPassword("Secret@2026!", DefaultPBKDF2Iter)
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}
	if !strings.HasPrefix(hash, "pbkdf2_sha256$120000$") {
		t.Fatalf("unexpected hash format: %s", hash)
	}
	if !VerifyPassword("Secret@2026!", hash) {
		t.Fatal("VerifyPassword should succeed for matching password")
	}
	if VerifyPassword("WrongPass", hash) {
		t.Fatal("VerifyPassword should fail for wrong password")
	}
	// Known vector matching C++ crypto.cpp (RFC 7914 / PBKDF2-HMAC-SHA256)
	// password="password", salt=0x73616c74 ("salt"), iter=1000
	if !VerifyPassword("password", "pbkdf2_sha256$1000$73616c74$632c2812e46d4604102ba7618e9d6d7d2f8128f6266b4a03264d2a0460b7dcb3") {
		t.Fatal("VerifyPassword failed RFC PBKDF2-HMAC-SHA256 known vector")
	}
}

func TestHotkeyParseAndNormalize(t *testing.T) {
	spec, err := ParseHotkey("ctrl + alt + k")
	if err != nil {
		t.Fatalf("ParseHotkey failed: %v", err)
	}
	if spec.String() != "Ctrl+Alt+K" {
		t.Fatalf("got %q want Ctrl+Alt+K", spec.String())
	}
	if _, err := ParseHotkey("Alt+F4"); err == nil {
		t.Fatal("Alt+F4 must be rejected")
	}
	if _, err := ParseHotkey("Shift+K"); err == nil {
		t.Fatal("Shift-only hotkey must be rejected")
	}
	if got := NormalizeHotkey("invalid", DefaultHotkey); got != DefaultHotkey {
		t.Fatalf("got %q want %q", got, DefaultHotkey)
	}
}

func TestAuthThrottleBackoff(t *testing.T) {
	var th AuthThrottle
	for i := 1; i <= 4; i++ {
		if sec := th.RecordFailure(); sec != 0 {
			t.Fatalf("failure %d should have 0 cooldown, got %d", i, sec)
		}
	}
	if sec := th.RecordFailure(); sec != 30 {
		t.Fatalf("5th failure should trigger 30s cooldown, got %d", sec)
	}
	if sec := th.RecordFailure(); sec != 60 {
		t.Fatalf("6th failure should trigger 60s cooldown, got %d", sec)
	}
	if sec := th.RecordFailure(); sec != 300 {
		t.Fatalf("7th failure should trigger 300s cooldown, got %d", sec)
	}
	th.RecordSuccess()
	if rem := th.RemainingCooldownSeconds(); rem != 0 {
		t.Fatalf("after success cooldown should be 0, got %d", rem)
	}
}

func TestStateStoreCompatibilityAndBackup(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "state.json")

	// Simulate existing v2.3.0 state.json written by C++ StateStore (with legacy Ctrl+Alt+L)
	legacyJSON := `{
  "version": 1,
  "master_hash": "pbkdf2_sha256$1000$73616c74$632c2812e46d4604102ba7618e9d6d7d2f8128f6266b4a03264d2a0460b7dcb3",
  "settings": {
    "auto_relock_minutes": 10,
    "require_master_on_open": true,
    "allow_in_place_fallback": false,
    "hotkey": "Ctrl+Alt+L"
  },
  "entries": [
    {
      "id": "abcdef0123456789abcdef0123456789",
      "path": "/tmp/demo_secret",
      "locked": true,
      "use_custom_password": false,
      "custom_hash": "",
      "orig_mode": 493,
      "orig_uid": 1000,
      "orig_gid": 1000,
      "set_immutable": false,
      "win_sid": "S-1-1-0",
      "vault_path": "/tmp/vault/abcdef",
      "stub_path": "/tmp/demo_secret.desktop",
      "in_place_acl": false,
      "win_orig_attrs": 16,
      "created_at": 1700000000,
      "last_unlocked_at": 0
    }
  ]
}`
	if err := os.WriteFile(path, []byte(legacyJSON), 0600); err != nil {
		t.Fatalf("write legacy state failed: %v", err)
	}

	store := NewStateStore(path)
	if err := store.Load(); err != nil {
		t.Fatalf("Load failed: %v", err)
	}
	if !store.HasMasterPassword() || !VerifyPassword("password", store.State.MasterHash) {
		t.Fatal("master hash failed to load or verify")
	}
	if store.State.Settings.Hotkey != DefaultHotkey {
		t.Fatalf("legacy Ctrl+Alt+L should migrate to %s, got %s", DefaultHotkey, store.State.Settings.Hotkey)
	}
	if len(store.State.Entries) != 1 || store.State.Entries[0].ID != "abcdef0123456789abcdef0123456789" {
		t.Fatalf("entries mismatch: %+v", store.State.Entries)
	}

	// Corrupt primary file and ensure .bak restores cleanly
	if err := os.WriteFile(path, []byte("{corrupted"), 0600); err != nil {
		t.Fatalf("corrupt primary failed: %v", err)
	}
	store2 := NewStateStore(path)
	if err := store2.Load(); err != nil {
		t.Fatalf("Load from .bak failed: %v", err)
	}
	if len(store2.State.Entries) != 1 {
		t.Fatalf("expected 1 entry recovered from .bak, got %d", len(store2.State.Entries))
	}
}
