package backend

import (
	"os"
	"path/filepath"
	"strings"

	"dirlocker/internal/core"
)

// Engine defines the platform-neutral operations implemented by Linux (dirlockerd IPC)
// and Windows (direct local StateStore + NTFS Vault/ACL).
type Engine interface {
	StatePath() string
	Status() (*core.StatusResponse, error)
	Query(path string) (*core.QueryResponse, error)
	SetMasterPassword(oldPassword, newPassword string) error
	VerifyMaster(password string) (bool, error)
	UpdateSettings(password string, settings core.Settings) error
	AddAndLock(path string, useCustom bool, customPassword string) (*core.PublicDirEntry, error)
	Lock(id, path string) error
	LockAll() (*core.LockAllResult, error)
	Unlock(id, path, password string) (*core.PublicDirEntry, error)
	ChangeDirPassword(id, currentPassword string, useCustom bool, newCustomPassword string) error
	Remove(id, password string) error
	Diagnose(password string) (*core.HealthReport, error)
	Repair(password string, cleanDeadRecords bool) (*core.RepairResult, error)
	OpenFolder(path string) error
	ReadStubFile(stubPath string) (entryID string, origPath string, ok bool)
	TryUpgradeInPlace() int
	HandleMaintenanceCLI(args []string) (handled bool, exitCode int)
	EnsureShellIntegration()
}

// PlatformHooks provides OS-native tray, global hotkey, idle time, and window corner helpers.
type PlatformHooks interface {
	Start(onShow func(), onLockAll func(), onQuit func())
	Stop()
	RegisterHotkey(hotkey string) (active bool, warning string)
	TestHotkey(hotkey string) (ok bool, msg string)
	IdleSeconds() (int, bool)
	TrayAvailable() bool
	ApplyWindowRoundedCorners()
	OpenURL(url string) error
}

// FilterDirectories validates and deduplicates a list of paths, keeping only existing directories
// or DirLocker stub files (.dlock / .desktop).
func FilterDirectories(paths []string) []string {
	seen := make(map[string]struct{}, len(paths))
	out := make([]string, 0, len(paths))
	for _, raw := range paths {
		cleaned := strings.Trim(strings.TrimSpace(raw), `"'`)
		if cleaned == "" {
			continue
		}
		if strings.HasPrefix(cleaned, "file://") {
			cleaned = strings.TrimPrefix(cleaned, "file://")
		}
		abs, err := filepath.Abs(cleaned)
		if err != nil {
			continue
		}
		info, err := os.Stat(abs)
		if err != nil || !info.IsDir() {
			continue
		}
		norm := core.NormalizePathLexical(abs)
		key := strings.ToLower(norm)
		if _, dup := seen[key]; dup {
			continue
		}
		seen[key] = struct{}{}
		out = append(out, norm)
	}
	return out
}
